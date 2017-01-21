/* $Id: DrvHostCoreAudio.cpp $ */
/** @file
 * VBox audio devices - Mac OS X CoreAudio audio driver.
 */

/*
 * Copyright (C) 2010-2016 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DRV_HOST_AUDIO
#include <VBox/log.h>
#include <VBox/vmm/pdmaudioifs.h>

#include "DrvAudio.h"
#include "AudioMixBuffer.h"

#include "VBoxDD.h"

#include <iprt/asm.h>
#include <iprt/cdefs.h>
#include <iprt/circbuf.h>
#include <iprt/mem.h>

#include <iprt/uuid.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioConverter.h>
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
# include <AudioToolbox/AudioToolbox.h>
#endif

#if 0
# include <iprt/file.h>
# define DEBUG_DUMP_PCM_DATA
# ifdef RT_OS_WINDOWS
#  define DEBUG_DUMP_PCM_DATA_PATH "c:\\temp\\"
# else
#  define DEBUG_DUMP_PCM_DATA_PATH "/tmp/"
# endif
#endif

/** @todo
 * - Maybe make sure the threads are immediately stopped if playing/recording stops.
 */

/*
 * Most of this is based on:
 * http://developer.apple.com/mac/library/technotes/tn2004/tn2097.html
 * http://developer.apple.com/mac/library/technotes/tn2002/tn2091.html
 * http://developer.apple.com/mac/library/qa/qa2007/qa1533.html
 * http://developer.apple.com/mac/library/qa/qa2001/qa1317.html
 * http://developer.apple.com/mac/library/documentation/AudioUnit/Reference/AUComponentServicesReference/Reference/reference.html
 */


/*******************************************************************************
 *
 * Helper function section
 *
 ******************************************************************************/

/* Move these down below the internal function prototypes... */

static void coreAudioPrintASBD(const char *pszDesc, const AudioStreamBasicDescription *pASBD)
{
    char pszSampleRate[32];
    LogRel2(("CoreAudio: %s description:\n", pszDesc));
    LogRel2(("CoreAudio: Format ID: %RU32 (%c%c%c%c)\n", pASBD->mFormatID,
             RT_BYTE4(pASBD->mFormatID), RT_BYTE3(pASBD->mFormatID),
             RT_BYTE2(pASBD->mFormatID), RT_BYTE1(pASBD->mFormatID)));
    LogRel2(("CoreAudio: Flags: %RU32", pASBD->mFormatFlags));
    if (pASBD->mFormatFlags & kAudioFormatFlagIsFloat)
        LogRel2((" Float"));
    if (pASBD->mFormatFlags & kAudioFormatFlagIsBigEndian)
        LogRel2((" BigEndian"));
    if (pASBD->mFormatFlags & kAudioFormatFlagIsSignedInteger)
        LogRel2((" SignedInteger"));
    if (pASBD->mFormatFlags & kAudioFormatFlagIsPacked)
        LogRel2((" Packed"));
    if (pASBD->mFormatFlags & kAudioFormatFlagIsAlignedHigh)
        LogRel2((" AlignedHigh"));
    if (pASBD->mFormatFlags & kAudioFormatFlagIsNonInterleaved)
        LogRel2((" NonInterleaved"));
    if (pASBD->mFormatFlags & kAudioFormatFlagIsNonMixable)
        LogRel2((" NonMixable"));
    if (pASBD->mFormatFlags & kAudioFormatFlagsAreAllClear)
        LogRel2((" AllClear"));
    LogRel2(("\n"));
    snprintf(pszSampleRate, 32, "%.2f", (float)pASBD->mSampleRate); /** @todo r=andy Use RTStrPrint*. */
    LogRel2(("CoreAudio: SampleRate      : %s\n", pszSampleRate));
    LogRel2(("CoreAudio: ChannelsPerFrame: %RU32\n", pASBD->mChannelsPerFrame));
    LogRel2(("CoreAudio: FramesPerPacket : %RU32\n", pASBD->mFramesPerPacket));
    LogRel2(("CoreAudio: BitsPerChannel  : %RU32\n", pASBD->mBitsPerChannel));
    LogRel2(("CoreAudio: BytesPerFrame   : %RU32\n", pASBD->mBytesPerFrame));
    LogRel2(("CoreAudio: BytesPerPacket  : %RU32\n", pASBD->mBytesPerPacket));
}

static void coreAudioPCMPropsToASBD(PDMAUDIOPCMPROPS *pPCMProps, AudioStreamBasicDescription *pASBD)
{
    AssertPtrReturnVoid(pPCMProps);
    AssertPtrReturnVoid(pASBD);

    RT_BZERO(pASBD, sizeof(AudioStreamBasicDescription));

    pASBD->mFormatID         = kAudioFormatLinearPCM;
    pASBD->mFormatFlags      = kAudioFormatFlagIsPacked;
    pASBD->mFramesPerPacket  = 1; /* For uncompressed audio, set this to 1. */
    pASBD->mSampleRate       = (Float64)pPCMProps->uHz;
    pASBD->mChannelsPerFrame = pPCMProps->cChannels;
    pASBD->mBitsPerChannel   = pPCMProps->cBits;
    if (pPCMProps->fSigned)
        pASBD->mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    pASBD->mBytesPerFrame    = pASBD->mChannelsPerFrame * (pASBD->mBitsPerChannel / 8);
    pASBD->mBytesPerPacket   = pASBD->mFramesPerPacket * pASBD->mBytesPerFrame;
}

static int coreAudioStreamCfgToASBD(PPDMAUDIOSTREAMCFG pCfg, AudioStreamBasicDescription *pASBD)
{
    AssertPtrReturn(pCfg,  VERR_INVALID_PARAMETER);
    AssertPtrReturn(pASBD, VERR_INVALID_PARAMETER);

    PDMAUDIOPCMPROPS Props;
    int rc = DrvAudioHlpStreamCfgToProps(pCfg, &Props);
    if (RT_SUCCESS(rc))
        coreAudioPCMPropsToASBD(&Props, pASBD);

    return rc;
}

static int coreAudioASBDToStreamCfg(AudioStreamBasicDescription *pASBD, PPDMAUDIOSTREAMCFG pCfg)
{
    AssertPtrReturn(pASBD, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pCfg,  VERR_INVALID_PARAMETER);

    pCfg->cChannels     = pASBD->mChannelsPerFrame;
    pCfg->uHz           = (uint32_t)pASBD->mSampleRate;
    pCfg->enmEndianness = PDMAUDIOENDIANNESS_LITTLE;

    int rc = VINF_SUCCESS;

    if (pASBD->mFormatFlags & kAudioFormatFlagIsSignedInteger)
    {
        switch (pASBD->mBitsPerChannel)
        {
            case 8:  pCfg->enmFormat = PDMAUDIOFMT_S8;  break;
            case 16: pCfg->enmFormat = PDMAUDIOFMT_S16; break;
            case 32: pCfg->enmFormat = PDMAUDIOFMT_S32; break;
            default: rc = VERR_NOT_SUPPORTED;           break;
        }
    }
    else
    {
        switch (pASBD->mBitsPerChannel)
        {
            case 8:  pCfg->enmFormat = PDMAUDIOFMT_U8;  break;
            case 16: pCfg->enmFormat = PDMAUDIOFMT_U16; break;
            case 32: pCfg->enmFormat = PDMAUDIOFMT_U32; break;
            default: rc = VERR_NOT_SUPPORTED;           break;
        }
    }

    AssertRC(rc);
    return rc;
}

#ifndef VBOX_WITH_AUDIO_CA_QUEUES
static OSStatus coreAudioSetFrameBufferSize(AudioDeviceID deviceID, bool fInput, UInt32 cReqSize, UInt32 *pcActSize)
{
    AudioObjectPropertyScope propScope = fInput
                                       ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
    AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyBufferFrameSize, propScope,
                                           kAudioObjectPropertyElementMaster };

    /* First try to set the new frame buffer size. */
    OSStatus err = AudioObjectSetPropertyData(deviceID, &propAdr, 0, NULL, sizeof(cReqSize), &cReqSize);

    /* Check if it really was set. */
    UInt32 cSize = sizeof(*pcActSize);
    err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &cSize, pcActSize);
    if (RT_UNLIKELY(err != noErr))
        return err;

    /* If both sizes are the same, we are done. */
    if (cReqSize == *pcActSize)
        return noErr;

    /* If not we have to check the limits of the device. First get the size of
       the buffer size range property. */
    propAdr.mSelector = kAudioDevicePropertyBufferSizeRange;
    err = AudioObjectGetPropertyDataSize(deviceID, &propAdr, 0, NULL, &cSize);
    if (RT_UNLIKELY(err != noErr))
        return err;

    Assert(cSize);
    AudioValueRange *pRange = (AudioValueRange *)RTMemAllocZ(cSize);
    if (pRange)
    {
        err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &cSize, pRange);
        if (err == noErr)
        {
            Float64 cMin = -1;
            Float64 cMax = -1;
            for (size_t a = 0; a < cSize / sizeof(AudioValueRange); a++)
            {
                /* Search for the absolute minimum. */
                if (   pRange[a].mMinimum < cMin
                    || cMin == -1)
                    cMin = pRange[a].mMinimum;

                /* Search for the best maximum which isn't bigger than cReqSize. */
                if (pRange[a].mMaximum < cReqSize)
                {
                    if (pRange[a].mMaximum > cMax)
                        cMax = pRange[a].mMaximum;
                }
            }
            if (cMax == -1)
                cMax = cMin;
            cReqSize = cMax;

            /* First try to set the new frame buffer size. */
            propAdr.mSelector = kAudioDevicePropertyBufferFrameSize;
            err = AudioObjectSetPropertyData(deviceID, &propAdr, 0, NULL, sizeof(cReqSize), &cReqSize);
            if (err == noErr)
            {
                /* Check if it really was set. */
                cSize = sizeof(*pcActSize);
                err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &cSize, pcActSize);
            }
        }

        RTMemFree(pRange);
    }
    else
        err = notEnoughMemoryErr;

    return err;
}

DECL_FORCE_INLINE(bool) coreAudioIsRunning(AudioDeviceID deviceID)
{
    AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMaster };
    UInt32 uFlag = 0;
    UInt32 uSize = sizeof(uFlag);
    OSStatus err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &uSize, &uFlag);
    if (err != kAudioHardwareNoError)
        LogRel(("CoreAudio: Could not determine whether the device is running (%RI32)\n", err));

    return (uFlag >= 1);
}

static int coreAudioCFStringToCString(const CFStringRef pCFString, char **ppszString)
{
    CFIndex cLen = CFStringGetLength(pCFString) + 1;
    char *pszResult = (char *)RTMemAllocZ(cLen * sizeof(char));
    if (!CFStringGetCString(pCFString, pszResult, cLen, kCFStringEncodingUTF8))
    {
        RTMemFree(pszResult);
        return VERR_NOT_FOUND;
    }

    *ppszString = pszResult;
    return VINF_SUCCESS;
}
#endif /* VBOX_WITH_AUDIO_CA_QUEUES */

#if 0 /* unused */
static AudioDeviceID coreAudioDeviceUIDtoID(const char* pszUID)
{
    /* Create a CFString out of our CString. */
    CFStringRef strUID = CFStringCreateWithCString(NULL, pszUID, kCFStringEncodingMacRoman);

    /* Fill the translation structure. */
    AudioDeviceID deviceID;

    AudioValueTranslation translation;
    translation.mInputData      = &strUID;
    translation.mInputDataSize  = sizeof(CFStringRef);
    translation.mOutputData     = &deviceID;
    translation.mOutputDataSize = sizeof(AudioDeviceID);

    /* Fetch the translation from the UID to the device ID. */
    AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMaster };

    UInt32 uSize = sizeof(AudioValueTranslation);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdr, 0, NULL, &uSize, &translation);

    /* Release the temporary CFString */
    CFRelease(strUID);

    if (RT_LIKELY(err == noErr))
        return deviceID;

    /* Return the unknown device on error. */
    return kAudioDeviceUnknown;
}
#endif /* unused */


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** @name Initialization status indicator used for the recreation of the AudioUnits.
 * @{ */
#define CA_STATUS_UNINIT    UINT32_C(0) /**< The device is uninitialized */
#define CA_STATUS_IN_INIT   UINT32_C(1) /**< The device is currently initializing */
#define CA_STATUS_INIT      UINT32_C(2) /**< The device is initialized */
#define CA_STATUS_IN_UNINIT UINT32_C(3) /**< The device is currently uninitializing */
#define CA_STATUS_REINIT    UINT32_C(4) /**< The device has to be reinitialized */
/** @} */


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
#ifdef VBOX_WITH_AUDIO_CA_CONVERTER
/* Error code which indicates "End of data" */
static const OSStatus g_caConverterEOFDErr = 0x656F6664; /* 'eofd' */
#endif


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/* Prototypes needed for COREAUDIOSTREAMCBCTX. */
struct COREAUDIOSTREAMIN;
typedef struct COREAUDIOSTREAMIN *PCOREAUDIOSTREAMIN;
struct COREAUDIOSTREAMOUT;
typedef struct COREAUDIOSTREAMOUT *PCOREAUDIOSTREAMOUT;

/**
 * Host Coreaudio driver instance data.
 * @implements PDMIAUDIOCONNECTOR
 */
typedef struct DRVHOSTCOREAUDIO
{
    /** Pointer to the driver instance structure. */
    PPDMDRVINS    pDrvIns;
    /** Pointer to host audio interface. */
    PDMIHOSTAUDIO IHostAudio;
} DRVHOSTCOREAUDIO, *PDRVHOSTCOREAUDIO;

/** Converts a pointer to DRVHOSTCOREAUDIO::IHostAudio to a PDRVHOSTCOREAUDIO. */
#define PDMIHOSTAUDIO_2_DRVHOSTCOREAUDIO(pInterface) RT_FROM_MEMBER(pInterface, DRVHOSTCOREAUDIO, IHostAudio)

/**
 * Simple structure for maintaining a stream's callback context.
 ** @todo Remove this as soon as we have unified input/output streams in this backend.
 */
typedef struct COREAUDIOSTREAM
{
    /** Pointer to driver instance. */
    PDRVHOSTCOREAUDIO           pThis;
    /** The stream's direction. */
    PDMAUDIODIR                 enmDir;
    union
    {
        /** Pointer to self, if it's an input stream. */
        PCOREAUDIOSTREAMIN      pIn;
        /** Pointer to self, if it's an output stream. */
        PCOREAUDIOSTREAMOUT     pOut;
    };
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    /** The stream's thread handle for maintaining the audio queue. */
    RTTHREAD                    hThread;
    /** Flag indicating to start a stream's data processing. */
    bool                        fRun;
    /** Whether the stream is in a running (active) state or not.
     *  For playback streams this means that audio data can be (or is being) played,
     *  for capturing streams this means that audio data is being captured (if available). */
    bool                        fIsRunning;
    /** Thread shutdown indicator. */
    bool                        fShutdown;
    /** Critical section for serializing access between thread + callbacks. */
    RTCRITSECT                  CritSect;
    /** The actual audio queue being used. */
    AudioQueueRef               audioQueue;
    /** The audio buffers which are used with the above audio queue. */
    AudioQueueBufferRef         audioBuffer[3];
    /** The acquired (final) audio format for this stream. */
    AudioStreamBasicDescription asbdStream;
    /** The device' UUID. */
    CFStringRef                 UUID;
    /** An internal ring buffer for transferring data from/to the rendering callbacks. */
    PRTCIRCBUF                  pCircBuf;
#endif
} COREAUDIOSTREAM, *PCOREAUDIOSTREAM;

/**
 * Structure for keeping a conversion callback context.
 * This is needed when using an audio converter during input/output processing.
 */
typedef struct COREAUDIOCONVCBCTX
{
    /** Pointer to stream context this converter callback context
     *  is bound to. */
    /** @todo Remove this as soon as we have unified input/output streams in this backend. */
    COREAUDIOSTREAM             Stream;
    /** Source stream description. */
    AudioStreamBasicDescription asbdSrc;
    /** Destination stream description. */
    AudioStreamBasicDescription asbdDst;
    /** Native buffer list used for rendering the source audio data into. */
    AudioBufferList             bufLstSrc;
    /** Total packet conversion count. */
    UInt32                      uPacketCnt;
    /** Current packet conversion index. */
    UInt32                      uPacketIdx;
} COREAUDIOCONVCBCTX, *PCOREAUDIOCONVCBCTX;

/** @todo Unify COREAUDIOSTREAMOUT / COREAUDIOSTREAMIN. */
typedef struct COREAUDIOSTREAMOUT
{
    /** Host output stream.
     *  Note: Always must come first in this structure! */
    PDMAUDIOSTREAM              Stream;
    /** Common stream data.
     *  @todo Remove this as soon as we have unified input/output streams in this backend. */
    COREAUDIOSTREAM             Common;
    /** Stream description which is default on the device. */
    AudioStreamBasicDescription deviceFormat;
    /** Stream description which is selected for using with VBox. */
    AudioStreamBasicDescription streamFormat;
    /** The audio device ID of the currently used device. */
    AudioDeviceID               deviceID;
    /** The AudioUnit being used. */
    AudioUnit                   audioUnit;
    /** A ring buffer for transferring data to the playback thread. */
    PRTCIRCBUF                  pCircBuf;
    /** Initialization status tracker. Used when some of the device parameters
     *  or the device itself is changed during the runtime. */
    volatile uint32_t           status;
    /** Flag whether the "default device changed" listener was registered. */
    bool                        fDefDevChgListReg;
    /** Flag whether the "device state changed" listener was registered. */
    bool                        fDevStateChgListReg;
} COREAUDIOSTREAMOUT, *PCOREAUDIOSTREAMOUT;

typedef struct COREAUDIOSTREAMIN
{
    /** Host input stream.
     *  Note: Always must come first in this structure! */
    PDMAUDIOSTREAM              Stream;
    /** Common stream data.
     *  @todo Remove this as soon as we have unified input/output streams in this backend. */
    COREAUDIOSTREAM             Common;
    /** Stream description which is default on the device. */
    AudioStreamBasicDescription deviceFormat;
    /** Stream description which is selected for using with VBox. */
    AudioStreamBasicDescription streamFormat;
    /** The audio device ID of the currently used device. */
    AudioDeviceID               deviceID;
    /** The AudioUnit used. */
    AudioUnit                   audioUnit;
    /** A ring buffer for transferring data to the playback thread. */
    PRTCIRCBUF                  pCircBuf;
    /** The audio converter if necessary. NULL if no converter is being used. */
    AudioConverterRef           pConverter;
    /** Callback context for the audio converter. */
    COREAUDIOCONVCBCTX          convCbCtx;
    /** The ratio between the device & the stream sample rate. */
    Float64                     sampleRatio;
    /** Initialization status tracker. Used when some of the device parameters
     *  or the device itself is changed during the runtime. */
    volatile uint32_t           status;
    /** Flag whether the "default device changed" listener was registered. */
    bool                        fDefDevChgListReg;
    /** Flag whether the "device state changed" listener was registered. */
    bool                        fDevStateChgListReg;
} COREAUDIOSTREAMIN, *PCOREAUDIOSTREAMIN;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static OSStatus coreAudioPlaybackAudioDevicePropertyChanged(AudioObjectID propertyID, UInt32 cAddresses, const AudioObjectPropertyAddress properties[], void *pvUser);
#ifndef VBOX_WITH_AUDIO_CA_QUEUES
static int coreAudioStreamInitIn(PCOREAUDIOSTREAMIN pStreamIn, PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq);
static int coreAudioStreamInitOut(PCOREAUDIOSTREAMOUT pStreamOut, PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq);
static OSStatus coreAudioPlaybackCb(void *pvUser, AudioUnitRenderActionFlags *pActionFlags, const AudioTimeStamp *pAudioTS, UInt32 uBusID, UInt32 cFrames, AudioBufferList* pBufData);
#endif

static int coreAudioControlStreamIn(PCOREAUDIOSTREAMIN pCAStreamIn, PDMAUDIOSTREAMCMD enmStreamCmd);
static int coreAudioControlStreamOut(PCOREAUDIOSTREAMOUT pCAStreamOut, PDMAUDIOSTREAMCMD enmStreamCmd);

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
static void coreAudioInputQueueCb(void *pvUser, AudioQueueRef audioQueue, AudioQueueBufferRef audioBuffer, const AudioTimeStamp *pAudioTS, UInt32 cPacketDesc, const AudioStreamPacketDescription *paPacketDesc);
static void coreAudioOutputQueueCb(void *pvUser, AudioQueueRef audioQueue, AudioQueueBufferRef audioBuffer);
#endif


#ifdef VBOX_WITH_AUDIO_CA_CONVERTER
/**
 * Initializes a conversion callback context.
 *
 * @return  IPRT status code.
 * @param   pConvCbCtx          Conversion callback context to initialize.
 * @param   pASBDSrc            Input (source) stream description to use.
 * @param   pASBDDst            Output (destination) stream description to use.
 */
static int coreAudioInitConvCbCtx(PCOREAUDIOCONVCBCTX pConvCbCtx,
                                  AudioStreamBasicDescription *pASBDSrc, AudioStreamBasicDescription *pASBDDst)
{
    AssertPtrReturn(pConvCbCtx, VERR_INVALID_POINTER);
    AssertPtrReturn(pASBDSrc,   VERR_INVALID_POINTER);
    AssertPtrReturn(pASBDDst,   VERR_INVALID_POINTER);

    memcpy(&pConvCbCtx->asbdSrc, pASBDSrc, sizeof(AudioStreamBasicDescription));
    memcpy(&pConvCbCtx->asbdDst, pASBDDst, sizeof(AudioStreamBasicDescription));

    /* Create the AudioBufferList structure with one buffer. */
    pConvCbCtx->bufLstSrc.mNumberBuffers              = 1;

    /* Initialize the conversion buffer. */
    pConvCbCtx->bufLstSrc.mBuffers[0].mNumberChannels = pConvCbCtx->asbdSrc.mChannelsPerFrame;
    pConvCbCtx->bufLstSrc.mBuffers[0].mDataByteSize   = 0;
    pConvCbCtx->bufLstSrc.mBuffers[0].mData           = NULL;

    return VINF_SUCCESS;
}

/**
 * Uninitializes a conversion callback context.
 *
 * @return  IPRT status code.
 * @param   pConvCbCtx          Conversion callback context to uninitialize.
 */
static void coreAudioUninitConvCbCtx(PCOREAUDIOCONVCBCTX pConvCbCtx)
{
    AssertPtrReturnVoid(pConvCbCtx);

    RT_ZERO(pConvCbCtx->asbdSrc);
    RT_ZERO(pConvCbCtx->asbdDst);

    pConvCbCtx->bufLstSrc.mNumberBuffers = 0;
}
#endif /* VBOX_WITH_AUDIO_CA_CONVERTER */

/**
 * Does a (Re-)enumeration of the host's playback + recording devices.
 *
 * @return  IPRT status code.
 * @param   pThis               Host audio driver instance.
 * @param   pCfg                Where to store the enumeration results.
 * @param   fEnum               Enumeration flags.
 */
static int coreAudioDevicesEnumerate(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOBACKENDCFG pCfg, bool fIn, uint32_t fEnum)
{
    RT_NOREF(fEnum);
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    /* pCfg is optional. */

    int rc = VINF_SUCCESS;

    uint8_t cDevs = 0;

    do
    {
        AudioObjectPropertyAddress propAdrDevList = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                                      kAudioObjectPropertyElementMaster };
        UInt32 uSize = 0;
        OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propAdrDevList, 0, NULL, &uSize);
        if (err != kAudioHardwareNoError)
            break;

        AudioDeviceID *pDevIDs = (AudioDeviceID *)alloca(uSize);
        if (pDevIDs == NULL)
            break;

        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdrDevList, 0, NULL, &uSize, pDevIDs);
        if (err != kAudioHardwareNoError)
            break;

        UInt32 cDevices = uSize / sizeof (AudioDeviceID);
        for (UInt32 i = 0; i < cDevices; i++)
        {
            AudioDeviceID curDevID = pDevIDs[i];

            /* Check if the device is valid. */
            AudioObjectPropertyAddress propAddrCfg = { kAudioDevicePropertyStreamConfiguration,
                                                       fIn ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                                                       kAudioObjectPropertyElementMaster };

            err = AudioObjectGetPropertyDataSize(curDevID, &propAddrCfg, 0, NULL, &uSize);
            if (err != noErr)
                continue;

            AudioBufferList *pBufList = (AudioBufferList *)RTMemAlloc(uSize);
            if (!pBufList)
                continue;

            bool fIsValid = false;

            err = AudioObjectGetPropertyData(curDevID, &propAddrCfg, 0, NULL, &uSize, pBufList);
            if (err == noErr)
            {
                for (UInt32 a = 0; a < pBufList->mNumberBuffers; a++)
                {
                    fIsValid = pBufList->mBuffers[a].mNumberChannels > 0;
                    if (fIsValid)
                        break;
                }
            }

            if (pBufList)
            {
                RTMemFree(pBufList);
                pBufList = NULL;
            }

            if (!fIsValid)
                continue;

            /* Resolve the device's name. */
            AudioObjectPropertyAddress propAddrName = { kAudioObjectPropertyName,
                                                        fIn ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                                                        kAudioObjectPropertyElementMaster };
            uSize = sizeof(CFStringRef);
            CFStringRef pcfstrName = NULL;

            err = AudioObjectGetPropertyData(curDevID, &propAddrName, 0, NULL, &uSize, &pcfstrName);
            if (err != kAudioHardwareNoError)
                continue;

            CFIndex uMax = CFStringGetMaximumSizeForEncoding(CFStringGetLength(pcfstrName), kCFStringEncodingUTF8) + 1;
            if (uMax)
            {
                char *pszName = (char *)RTStrAlloc(uMax);
                if (   pszName
                    && CFStringGetCString(pcfstrName, pszName, uMax, kCFStringEncodingUTF8))
                {
                    LogRel2(("CoreAudio: Found %s device '%s'\n", fIn ? "recording" : "playback", pszName));
                    cDevs++;
                }

                if (pszName)
                {
                    RTStrFree(pszName);
                    pszName = NULL;
                }
            }

            CFRelease(pcfstrName);
        }

    } while (0);

    if (fIn)
        LogRel2(("CoreAudio: Found %RU8 recording device(s)\n", cDevs));
    else
        LogRel2(("CoreAudio: Found %RU8 playback device(s)\n", cDevs));

    if (pCfg)
    {
        if (fIn)
            pCfg->cSources = cDevs;
        else
            pCfg->cSinks   = cDevs;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Updates this host driver's internal status, according to the global, overall input/output
 * state and all connected (native) audio streams.
 *
 * @param   pThis               Host audio driver instance.
 * @param   pCfg                Where to store the backend configuration. Optional.
 * @param   fEnum               Enumeration flags.
 */
int coreAudioUpdateStatusInternalEx(PDRVHOSTCOREAUDIO pThis, PPDMAUDIOBACKENDCFG pCfg, uint32_t fEnum)
{
    RT_NOREF(fEnum);
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    /* pCfg is optional. */

    PDMAUDIOBACKENDCFG Cfg;
    RT_ZERO(Cfg);

    Cfg.cbStreamOut    = sizeof(COREAUDIOSTREAMOUT);
    Cfg.cbStreamIn     = sizeof(COREAUDIOSTREAMIN);
    Cfg.cMaxStreamsIn  = UINT32_MAX;
    Cfg.cMaxStreamsOut = UINT32_MAX;

    int rc = coreAudioDevicesEnumerate(pThis, &Cfg, false /* fIn */, 0 /* fEnum */);
    AssertRC(rc);
    rc = coreAudioDevicesEnumerate(pThis, &Cfg, true /* fIn */, 0 /* fEnum */);
    AssertRC(rc);

    if (pCfg)
        memcpy(pCfg, &Cfg, sizeof(PDMAUDIOBACKENDCFG));

    LogFlowFuncLeaveRC(rc);
    return rc;
}


/**
 * Implements the OS X callback AudioObjectPropertyListenerProc.
 */
static OSStatus drvHostCoreAudioDeviceStateChanged(AudioObjectID propertyID,
                                                   UInt32 cAddresses,
                                                   const AudioObjectPropertyAddress paProperties[],
                                                   void *pvUser)
{
    RT_NOREF(propertyID, cAddresses, paProperties)
    LogFlowFunc(("propertyID=%u cAddresses=%u pvUser=%p\n", propertyID, cAddresses, pvUser));

    PCOREAUDIOSTREAM pCAStream = (PCOREAUDIOSTREAM)pvUser;
    AssertPtr(pCAStream);

    UInt32 uAlive = 1;
    UInt32 uSize  = sizeof(UInt32);

    AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMaster };

    AudioDeviceID deviceID = pCAStream->enmDir == PDMAUDIODIR_IN
                           ? pCAStream->pIn->deviceID : pCAStream->pOut->deviceID;

    OSStatus err = AudioObjectGetPropertyData(deviceID, &propAdr, 0, NULL, &uSize, &uAlive);

    bool fIsDead = false;

    if (err == kAudioHardwareBadDeviceError)
        fIsDead = true; /* Unplugged. */
    else if ((err == kAudioHardwareNoError) && (!RT_BOOL(uAlive)))
        fIsDead = true; /* Something else happened. */

    if (fIsDead)
    {
        switch (pCAStream->enmDir)
        {
            case PDMAUDIODIR_IN:
            {
                PCOREAUDIOSTREAMIN pStreamIn = pCAStream->pIn;

                /* We move the reinitialization to the next output event.
                 * This make sure this thread isn't blocked and the
                 * reinitialization is done when necessary only. */
                ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_REINIT);

                LogRel(("CoreAudio: Recording device stopped functioning\n"));
                break;
            }

            case PDMAUDIODIR_OUT:
            {
                PCOREAUDIOSTREAMOUT pStreamOut = pCAStream->pOut;

                /* We move the reinitialization to the next output event.
                 * This make sure this thread isn't blocked and the
                 * reinitialization is done when necessary only. */
                ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_REINIT);

                LogRel(("CoreAudio: Playback device stopped functioning\n"));
                break;
            }

            default:
                AssertMsgFailed(("Not implemented\n"));
                break;
        }
    }

    int rc2 = coreAudioDevicesEnumerate(pCAStream->pThis, NULL /* pCfg */, false /* fIn */, 0 /* fEnum */);
    AssertRC(rc2);
    rc2 = coreAudioDevicesEnumerate(pCAStream->pThis, NULL /* pCfg */, true /* fIn */, 0 /* fEnum */);
    AssertRC(rc2);

    return noErr;
}


/**
 * Implements the OS X callback AudioObjectPropertyListenerProc for getting
 * notified when the default recording/playback device has been changed.
 */
static OSStatus coreAudioDefaultDeviceChanged(AudioObjectID propertyID,
                                              UInt32 cAddresses,
                                              const AudioObjectPropertyAddress properties[],
                                              void *pvUser)
{
    RT_NOREF(propertyID);
    LogFlowFunc(("propertyID=%u cAddresses=%u pvUser=%p\n", propertyID, cAddresses, pvUser));

    PCOREAUDIOSTREAM pCAStream = (PCOREAUDIOSTREAM)pvUser;
    AssertPtr(pCAStream);

    OSStatus err = noErr;
    for (UInt32 idxAddress = 0; idxAddress < cAddresses; idxAddress++)
    {
        const AudioObjectPropertyAddress *pProperty = &properties[idxAddress];

        switch (pProperty->mSelector)
        {
            case kAudioHardwarePropertyDefaultInputDevice:
            {
                PCOREAUDIOSTREAMIN pStreamIn = pCAStream->pIn;
                AssertPtr(pStreamIn);

                /* This listener is called on every change of the hardware
                 * device. So check if the default device has really changed. */
                UInt32 uSize = sizeof(pStreamIn->deviceID);
                UInt32 uResp;
                err = AudioObjectGetPropertyData(kAudioObjectSystemObject, pProperty, 0, NULL, &uSize, &uResp);

                if (err == noErr)
                {
                    if (pStreamIn->deviceID != uResp)
                    {
                        LogRel(("CoreAudio: Default device for recording has changed\n"));

                        /* We move the reinitialization to the next input event.
                         * This make sure this thread isn't blocked and the
                         * reinitialization is done when necessary only. */
                        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_REINIT);
                    }
                }
                break;
            }

            case kAudioHardwarePropertyDefaultOutputDevice:
            {
                PCOREAUDIOSTREAMOUT pStreamOut = pCAStream->pOut;
                AssertPtr(pStreamOut);

                /* This listener is called on every change of the hardware
                 * device. So check if the default device has really changed. */
                AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultOutputDevice,
                                                       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };

                UInt32 uSize = sizeof(pStreamOut->deviceID);
                UInt32 uResp;
                err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdr, 0, NULL, &uSize, &uResp);

                if (err == noErr)
                {
                    if (pStreamOut->deviceID != uResp)
                    {
                        LogRel(("CoreAudio: Default device for playback has changed\n"));

                        /* We move the reinitialization to the next input event.
                         * This make sure this thread isn't blocked and the
                         * reinitialization is done when necessary only. */
                        ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_REINIT);
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    int rc2 = coreAudioDevicesEnumerate(pCAStream->pThis, NULL /* pCfg */, false /* fIn */, 0 /* fEnum */);
    AssertRC(rc2);
    rc2 = coreAudioDevicesEnumerate(pCAStream->pThis, NULL /* pCfg */, true /* fIn */, 0 /* fEnum */);
    AssertRC(rc2);

    /** @todo Implement callback notification here to let the audio connector / device emulation
     *        know that something has changed. */

    return noErr;
}

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
/**
 * Thread for a Core Audio stream's audio queue handling.
 * This thread is required per audio queue to pump data to/from the Core Audio stream and
 * handling its callbacks.
 *
 * @returns IPRT status code.
 * @param   hThreadSelf         Thread handle.
 * @param   pvUser              User argument.
 */
static DECLCALLBACK(int) coreAudioQueueThread(RTTHREAD hThreadSelf, void *pvUser)
{
    NOREF(hThreadSelf);

    PCOREAUDIOSTREAM pCAStream = (PCOREAUDIOSTREAM)pvUser;
    AssertPtr(pCAStream);

    LogFunc(("Starting pCAStream=%p\n", pCAStream));

    /*
     * Create audio queue.
     */
    OSStatus err;
    if (pCAStream->enmDir == PDMAUDIODIR_IN)
        err = AudioQueueNewInput(&pCAStream->asbdStream, coreAudioInputQueueCb, pCAStream /* pvData */,
                                 CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, 0, &pCAStream->audioQueue);
    else
        err = AudioQueueNewOutput(&pCAStream->asbdStream, coreAudioOutputQueueCb, pCAStream /* pvData */,
                                  CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, 0, &pCAStream->audioQueue);

    if (err != noErr)
        return VERR_GENERAL_FAILURE; /** @todo Fudge! */

    /*
     * Assign device to queue.
     */
    UInt32 uSize = sizeof(pCAStream->UUID);
    err = AudioQueueSetProperty(pCAStream->audioQueue, kAudioQueueProperty_CurrentDevice, &pCAStream->UUID, uSize);
    if (err != noErr)
        return VERR_GENERAL_FAILURE; /** @todo Fudge! */

    const size_t cbBufSize = _4K; /** @todo Make this configurable! */

    /*
     * Allocate audio buffers.
     */
    for (size_t i = 0; i < RT_ELEMENTS(pCAStream->audioBuffer); i++)
    {
        err = AudioQueueAllocateBuffer(pCAStream->audioQueue, cbBufSize, &pCAStream->audioBuffer[i]);
        if (err != noErr)
            break;
    }

    if (err != noErr)
        return VERR_GENERAL_FAILURE; /** @todo Fudge! */

    /* Signal the main thread before entering the main loop. */
    RTThreadUserSignal(RTThreadSelf());

    /*
     * Enter the main loop.
     */
    const bool fIn = pCAStream->enmDir == PDMAUDIODIR_IN;

    while (!ASMAtomicReadBool(&pCAStream->fShutdown))
    {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.10, 1);
    }

    /*
     * Cleanup.
     */
    if (fIn)
    {
        AudioQueueStop(pCAStream->audioQueue, 1);
    }
    else
    {
        AudioQueueStop(pCAStream->audioQueue, 0);
    }

    for (size_t i = 0; i < RT_ELEMENTS(pCAStream->audioBuffer); i++)
    {
        if (pCAStream->audioBuffer[i])
            AudioQueueFreeBuffer(pCAStream->audioQueue, pCAStream->audioBuffer[i]);
    }

    AudioQueueDispose(pCAStream->audioQueue, 1);

    LogFunc(("Ended pCAStream=%p\n", pCAStream));
    return VINF_SUCCESS;
}

/**
 * Processes input data of an audio queue buffer and stores it into a Core Audio stream.
 *
 * @returns IPRT status code.
 * @param   pCAStream           Core Audio stream to store input data into.
 * @param   audioBuffer         Audio buffer to process input data from.
 */
int coreAudioInputQueueProcBuffer(PCOREAUDIOSTREAM pCAStream, AudioQueueBufferRef audioBuffer)
{
    PRTCIRCBUF pCircBuf = pCAStream->pCircBuf;
    AssertPtr(pCircBuf);

    UInt8 *pvSrc = (UInt8 *)audioBuffer->mAudioData;
    UInt8 *pvDst = NULL;

    size_t cbWritten = 0;

    size_t cbToWrite = audioBuffer->mAudioDataByteSize;
    size_t cbLeft    = cbToWrite;

    while (cbLeft)
    {
        /* Try to acquire the necessary block from the ring buffer. */
        RTCircBufAcquireWriteBlock(pCircBuf, cbLeft, (void **)&pvDst, &cbToWrite);

        if (!cbToWrite)
            break;

        /* Copy the data from our ring buffer to the core audio buffer. */
        memcpy((UInt8 *)pvDst, pvSrc + cbWritten, cbToWrite);

        /* Release the read buffer, so it could be used for new data. */
        RTCircBufReleaseWriteBlock(pCircBuf, cbToWrite);

        cbWritten += cbToWrite;

        Assert(cbLeft >= cbToWrite);
        cbLeft -= cbToWrite;
    }

    Log3Func(("pCAStream=%p, cbBuffer=%RU32/%zu, cbWritten=%zu\n",
              pCAStream, audioBuffer->mAudioDataByteSize, audioBuffer->mAudioDataBytesCapacity, cbWritten));

    return VINF_SUCCESS;
}

/**
 * Input audio queue callback. Called whenever input data from the audio queue becomes available.
 *
 * @param   pvUser              User argument.
 * @param   audioQueue          Audio queue to process input data from.
 * @param   audioBuffer         Audio buffer to process input data from. Must be part of audio queue.
 * @param   pAudioTS            Audio timestamp.
 * @param   cPacketDesc         Number of packet descriptors.
 * @param   paPacketDesc        Array of packet descriptors.
 */
static DECLCALLBACK(void) coreAudioInputQueueCb(void *pvUser, AudioQueueRef audioQueue, AudioQueueBufferRef audioBuffer,
                                                const AudioTimeStamp *pAudioTS,
                                                UInt32 cPacketDesc, const AudioStreamPacketDescription *paPacketDesc)
{
    NOREF(pAudioTS);
    NOREF(cPacketDesc);
    NOREF(paPacketDesc);

    PCOREAUDIOSTREAM pCAStream = (PCOREAUDIOSTREAM)pvUser;
    AssertPtr(pCAStream);

    int rc = RTCritSectEnter(&pCAStream->CritSect);
    AssertRC(rc);

    rc = coreAudioInputQueueProcBuffer(pCAStream, audioBuffer);
    if (RT_SUCCESS(rc))
        AudioQueueEnqueueBuffer(audioQueue, audioBuffer, 0, NULL);

    rc = RTCritSectLeave(&pCAStream->CritSect);
    AssertRC(rc);
}

/**
 * Processes output data of a Core Audio stream into an audio queue buffer.
 *
 * @returns IPRT status code.
 * @param   pCAStream           Core Audio stream to process output data for.
 * @param   audioBuffer         Audio buffer to store data into.
 */
int coreAudioOutputQueueProcBuffer(PCOREAUDIOSTREAM pCAStream, AudioQueueBufferRef audioBuffer)
{
    PRTCIRCBUF pCircBuf = pCAStream->pCircBuf;
    AssertPtr(pCircBuf);

    size_t cbRead = 0;

    UInt8 *pvSrc = NULL;
    UInt8 *pvDst = (UInt8 *)audioBuffer->mAudioData;

    size_t cbToRead = RT_MIN(RTCircBufUsed(pCircBuf), audioBuffer->mAudioDataBytesCapacity);
    size_t cbLeft   = cbToRead;

    while (cbLeft)
    {
        /* Try to acquire the necessary block from the ring buffer. */
        RTCircBufAcquireReadBlock(pCircBuf, cbLeft, (void **)&pvSrc, &cbToRead);

        if (cbToRead)
        {
            /* Copy the data from our ring buffer to the core audio buffer. */
            memcpy((UInt8 *)pvDst + cbRead, pvSrc, cbToRead);
        }

        /* Release the read buffer, so it could be used for new data. */
        RTCircBufReleaseReadBlock(pCircBuf, cbToRead);

        if (!cbToRead)
            break;

        /* Move offset. */
        cbRead += cbToRead;
        Assert(cbRead <= audioBuffer->mAudioDataBytesCapacity);

        Assert(cbToRead <= cbLeft);
        cbLeft -= cbToRead;
    }

    audioBuffer->mAudioDataByteSize = cbRead;

    if (audioBuffer->mAudioDataByteSize < audioBuffer->mAudioDataBytesCapacity)
    {
        RT_BZERO((UInt8 *)audioBuffer->mAudioData + audioBuffer->mAudioDataByteSize,
                 audioBuffer->mAudioDataBytesCapacity - audioBuffer->mAudioDataByteSize);

        audioBuffer->mAudioDataByteSize = audioBuffer->mAudioDataBytesCapacity;
    }

    Log3Func(("pCAStream=%p, cbCapacity=%RU32, cbRead=%zu\n",
              pCAStream, audioBuffer->mAudioDataBytesCapacity, cbRead));

    return VINF_SUCCESS;
}

/**
 * Output audio queue callback. Called whenever an audio queue is ready to process more output data.
 *
 * @param   pvUser              User argument.
 * @param   audioQueue          Audio queue to process output data for.
 * @param   audioBuffer         Audio buffer to store output data in. Must be part of audio queue.
 */
static DECLCALLBACK(void) coreAudioOutputQueueCb(void *pvUser, AudioQueueRef audioQueue, AudioQueueBufferRef audioBuffer)
{
    PCOREAUDIOSTREAM pCAStream = (PCOREAUDIOSTREAM)pvUser;
    AssertPtr(pCAStream);

    int rc = RTCritSectEnter(&pCAStream->CritSect);
    AssertRC(rc);

    rc = coreAudioOutputQueueProcBuffer(pCAStream, audioBuffer);
    if (RT_SUCCESS(rc))
        AudioQueueEnqueueBuffer(audioQueue, audioBuffer, 0, NULL);

    rc = RTCritSectLeave(&pCAStream->CritSect);
    AssertRC(rc);
}

/**
 * Invalidates a Core Audio stream's audio queue.
 *
 * @returns IPRT status code.
 * @param   pCAStream           Core Audio stream to invalidate its queue for.
 */
static int coreAudioStreamInvalidateQueue(PCOREAUDIOSTREAM pCAStream)
{
    int rc = VINF_SUCCESS;

    for (size_t i = 0; i < RT_ELEMENTS(pCAStream->audioBuffer); i++)
    {
        AudioQueueBufferRef pBuf = pCAStream->audioBuffer[i];

        if (pCAStream->enmDir == PDMAUDIODIR_IN)
        {
            int rc2 = coreAudioInputQueueProcBuffer(pCAStream, pBuf);
            if (RT_SUCCESS(rc2))
            {
                AudioQueueEnqueueBuffer(pCAStream->audioQueue, pBuf, 0, NULL);
            }
        }
        else if (pCAStream->enmDir == PDMAUDIODIR_OUT)
        {
            int rc2 = coreAudioOutputQueueProcBuffer(pCAStream, pBuf);
            if (   RT_SUCCESS(rc2)
                && pBuf->mAudioDataByteSize)
            {
                AudioQueueEnqueueBuffer(pCAStream->audioQueue, pBuf, 0, NULL);
            }

            if (RT_SUCCESS(rc))
                rc = rc2;
        }
        else
            AssertFailed();
    }

    return rc;
}

/**
 * Initializes a Core Audio stream's audio queue.
 *
 * @returns IPRT status code.
 * @param   pCAStream           Core Audio stream to initialize audio queue for.
 * @param   fIn                 Whether this is an input or output queue.
 * @param   pCfgReq             Requested stream configuration.
 * @param   pCfgAcq             Acquired stream configuration on success.
 */
static int coreAudioStreamInitQueue(PCOREAUDIOSTREAM pCAStream, bool fIn, PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    RT_NOREF(pCfgAcq);

    LogFunc(("pCAStream=%p, pCfgReq=%p, pCfgAcq=%p\n", pCAStream, pCfgReq, pCfgAcq));

    AudioDeviceID deviceID = kAudioDeviceUnknown;

    /* Fetch the default audio device currently in use. */
    AudioObjectPropertyAddress propAdrDefaultDev = {   fIn
                                                     ? kAudioHardwarePropertyDefaultInputDevice
                                                     : kAudioHardwarePropertyDefaultOutputDevice,
                                                     kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
    UInt32 uSize = sizeof(deviceID);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdrDefaultDev, 0, NULL, &uSize, &deviceID);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Unable to determine default %s device (%RI32)\n",
                fIn ? "capturing" : "playback", err));
        return VERR_NOT_FOUND;
    }

    /* Get the device UUID. */
    AudioObjectPropertyAddress propAdrDevUUID = { kAudioDevicePropertyDeviceUID,
                                                  fIn ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                                                  kAudioObjectPropertyElementMaster };
    uSize = sizeof(pCAStream->UUID);
    err = AudioObjectGetPropertyData(deviceID, &propAdrDevUUID, 0, NULL, &uSize, &pCAStream->UUID);
    if (err != noErr)
    {
        LogRel(("CoreAudio: Failed to retrieve device UUID for device %RU32 (%RI32)\n", deviceID, err));
        return VERR_NOT_FOUND;
    }

    /* Create the recording device's out format based on our required audio settings. */
    int rc = coreAudioStreamCfgToASBD(pCfgReq, &pCAStream->asbdStream);
    if (RT_FAILURE(rc))
    {
        LogRel(("CoreAudio: Failed to convert requested %s format to native format (%Rrc)\n",
                fIn ? "input" : "output", rc));
        return rc;
    }

    pCAStream->enmDir = fIn ? PDMAUDIODIR_IN : PDMAUDIODIR_OUT;

    /* Assign device ID. */
    if (fIn)
    {
        AssertPtr(pCAStream->pIn);
        pCAStream->pIn->deviceID = deviceID;
    }
    else
    {
        AssertPtr(pCAStream->pOut);
        pCAStream->pOut->deviceID = deviceID;
    }

    coreAudioPrintASBD(  fIn
                       ? "Capturing queue format"
                       : "Playback queue format", &pCAStream->asbdStream);

    rc = RTCircBufCreate(&pCAStream->pCircBuf, 8096 << 1 /*pHstStrmIn->Props.cShift*/); /** @todo FIX THIS !!! */
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Start the thread.
     */
    rc = RTThreadCreate(&pCAStream->hThread, coreAudioQueueThread,
                        pCAStream /* pvUser */, 0 /* Default stack size */,
                        RTTHREADTYPE_DEFAULT, RTTHREADFLAGS_WAITABLE, "CAQUEUE");
    if (RT_SUCCESS(rc))
        rc = RTThreadUserWait(pCAStream->hThread, 10 * 1000 /* 10s timeout */);

    LogFunc(("Returning %Rrc\n", rc));
    return rc;
}

/**
 * Unitializes a Core Audio stream's audio queue.
 *
 * @returns IPRT status code.
 * @param   pCAStream           Core Audio stream to unitialize audio queue for.
 */
static int coreAudioStreamUninitQueue(PCOREAUDIOSTREAM pCAStream)
{
    LogFunc(("pCAStream=%p\n", pCAStream));

    int rc;

    if (pCAStream->hThread != NIL_RTTHREAD)
    {
        LogFunc(("Waiting for thread ...\n"));

        ASMAtomicXchgBool(&pCAStream->fShutdown, true);

        int rcThread;
        rc = RTThreadWait(pCAStream->hThread, 30 * 1000, &rcThread);
        if (RT_FAILURE(rc))
            return rc;

        NOREF(rcThread);
        LogFunc(("Thread stopped with %Rrc\n", rcThread));

        pCAStream->hThread = NIL_RTTHREAD;
    }

    if (pCAStream->pCircBuf)
    {
        RTCircBufDestroy(pCAStream->pCircBuf);
        pCAStream->pCircBuf = NULL;
    }

    LogFunc(("Returning\n"));
    return VINF_SUCCESS;
}
#endif /* VBOX_WITH_AUDIO_CA_QUEUES */

/**
 * Unitializes a Core Audio stream.
 *
 * @returns IPRT status code.
 * @param   pCAStream           Core Audio stream to uninitialize.
 */
static int coreAudioStreamUninit(PCOREAUDIOSTREAM pCAStream)
{
    LogFunc(("pCAStream=%p\n", pCAStream));

    int rc;

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    rc = coreAudioStreamUninitQueue(pCAStream);
#else
    rc = VINF_SUCCESS;
#endif

    return rc;
}

static int coreAudioStreamReinitIn(PCOREAUDIOSTREAM pCAStream)
{
    int rc = coreAudioStreamUninit(pCAStream);
    if (RT_SUCCESS(rc))
    {
        PDMAUDIOSTREAMCFG CfgAcq;
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
        rc = coreAudioASBDToStreamCfg(&pCAStream->asbdStream, &CfgAcq);
#else
        rc = coreAudioASBDToStreamCfg(&pCAStream->pIn->streamFormat, &CfgAcq);
#endif
        if (RT_SUCCESS(rc))
        {
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
            rc = coreAudioStreamInitQueue(pCAStream, true /* fInput */, &CfgAcq /* pCfgReq */, NULL /* pCfgAcq */);
#else
            /* Use the acquired stream configuration from the former initialization to
             * re-initialize the stream. */
            rc = coreAudioStreamInitIn(pCAStream->pIn, &CfgAcq /* pCfgReq */, NULL /* pCfgAcq */);
#endif
            if (RT_SUCCESS(rc))
                rc = coreAudioControlStreamIn(pCAStream->pIn, PDMAUDIOSTREAMCMD_ENABLE);

            if (RT_FAILURE(rc))
            {
                int rc2 = coreAudioStreamUninit(pCAStream);
                AssertRC(rc2);
            }
        }
    }

    if (RT_FAILURE(rc))
        LogRel(("CoreAudio: Unable to re-init input stream: %Rrc\n", rc));

    return rc;
}

static int coreAudioStreamReinitOut(PCOREAUDIOSTREAM pCAStream)
{
    int rc = coreAudioStreamUninit(pCAStream);
    if (RT_SUCCESS(rc))
    {
        PDMAUDIOSTREAMCFG CfgAcq;
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
        rc = coreAudioASBDToStreamCfg(&pCAStream->asbdStream, &CfgAcq);
#else
        rc = coreAudioASBDToStreamCfg(&pCAStream->pOut->streamFormat, &CfgAcq);
#endif
        if (RT_SUCCESS(rc))
        {
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
            rc = coreAudioStreamInitQueue(pCAStream, false /* fInput */, &CfgAcq /* pCfgReq */, NULL /* pCfgAcq */);
#else
            /* Use the acquired stream configuration from the former initialization to
             * re-initialize the stream. */
            rc = coreAudioStreamInitOut(pCAStream->pOut, &CfgAcq /* pCfgReq */, NULL /* pCfgAcq */);
#endif
            if (RT_SUCCESS(rc))
                rc = coreAudioControlStreamOut(pCAStream->pOut, PDMAUDIOSTREAMCMD_ENABLE);

            if (RT_FAILURE(rc))
            {
                int rc2 = coreAudioStreamUninit(pCAStream);
                AssertRC(rc2);
            }
        }
    }

    if (RT_FAILURE(rc))
        LogRel(("CoreAudio: Unable to re-init output stream: %Rrc\n", rc));

    return rc;
}

/**
 * Implements the OS X callback AudioObjectPropertyListenerProc for getting
 * notified when some of the properties of an audio device has changed.
 */
static OSStatus coreAudioRecordingAudioDevicePropertyChanged(AudioObjectID                     propertyID,
                                                             UInt32                            cAddresses,
                                                             const AudioObjectPropertyAddress  paProperties[],
                                                             void                             *pvUser)
{
    RT_NOREF(cAddresses, paProperties);
    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pvUser;

    switch (propertyID)
    {
#ifdef DEBUG
        case kAudioDeviceProcessorOverload:
        {
            LogFunc(("Processor overload detected!\n"));
            break;
        }
#endif /* DEBUG */
        case kAudioDevicePropertyNominalSampleRate:
        {
            LogRel(("CoreAudio: Recording sample rate changed\n"));

            /* We move the reinitialization to the next input event.
             * This make sure this thread isn't blocked and the
             * reinitialization is done when necessary only. */
            ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_REINIT);
            break;
        }

        default:
            break;
    }

    return noErr;
}

#ifdef VBOX_WITH_AUDIO_CA_CONVERTER
/**
 * Implements the OS X callback AudioConverterComplexInputDataProc for
 * converting audio input data from one format to another.
 */
static OSStatus coreAudioConverterCallback(AudioConverterRef              inAudioConverter,
                                           UInt32                        *ioNumberDataPackets,
                                           AudioBufferList               *ioData,
                                           AudioStreamPacketDescription **ppASPD,
                                           void                          *pvUser)
{
    RT_NOREF(inAudioConverter, ppASPD);
    AssertPtrReturn(ioNumberDataPackets, g_caConverterEOFDErr);
    AssertPtrReturn(ioData,              g_caConverterEOFDErr);

    PCOREAUDIOCONVCBCTX pConvCbCtx = (PCOREAUDIOCONVCBCTX)pvUser;
    AssertPtr(pConvCbCtx);

    /* Initialize values. */
    ioData->mBuffers[0].mNumberChannels = 0;
    ioData->mBuffers[0].mDataByteSize   = 0;
    ioData->mBuffers[0].mData           = NULL;

    /** @todo Check converter ID? */

    /** @todo Handled non-interleaved data by going through the full buffer list,
     *        not only through the first buffer like we do now. */

    Log3Func(("ioNumberDataPackets=%RU32\n", *ioNumberDataPackets));

    if (pConvCbCtx->uPacketIdx + *ioNumberDataPackets > pConvCbCtx->uPacketCnt)
    {
        Log3Func(("Limiting ioNumberDataPackets to %RU32\n", pConvCbCtx->uPacketCnt - pConvCbCtx->uPacketIdx));
        *ioNumberDataPackets = pConvCbCtx->uPacketCnt - pConvCbCtx->uPacketIdx;
    }

    if (*ioNumberDataPackets)
    {
        Assert(pConvCbCtx->bufLstSrc.mNumberBuffers == 1); /* Only one buffer for the source supported atm. */

        size_t cbOff   = pConvCbCtx->uPacketIdx * pConvCbCtx->asbdSrc.mBytesPerPacket;

        size_t cbAvail = RT_MIN(*ioNumberDataPackets * pConvCbCtx->asbdSrc.mBytesPerPacket,
                                pConvCbCtx->bufLstSrc.mBuffers[0].mDataByteSize - cbOff);

        void  *pvAvail = (uint8_t *)pConvCbCtx->bufLstSrc.mBuffers[0].mData + cbOff;

        Log3Func(("cbOff=%zu, cbAvail=%zu\n", cbOff, cbAvail));

        /* Set input data for the converter to use.
         * Note: For VBR (Variable Bit Rates) or interleaved data handling we need multiple buffers here. */
        ioData->mNumberBuffers = 1;

        ioData->mBuffers[0].mNumberChannels = pConvCbCtx->bufLstSrc.mBuffers[0].mNumberChannels;
        ioData->mBuffers[0].mDataByteSize   = cbAvail;
        ioData->mBuffers[0].mData           = pvAvail;

#ifdef DEBUG_DUMP_PCM_DATA
        RTFILE fh;
        int rc = RTFileOpen(&fh, DEBUG_DUMP_PCM_DATA_PATH "ca-converter-cb-input.pcm",
                            RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND | RTFILE_O_WRITE | RTFILE_O_DENY_NONE);
        if (RT_SUCCESS(rc))
        {
            RTFileWrite(fh, pvAvail, cbAvail, NULL);
            RTFileClose(fh);
        }
        else
            AssertFailed();
#endif

        pConvCbCtx->uPacketIdx += *ioNumberDataPackets;
        Assert(pConvCbCtx->uPacketIdx <= pConvCbCtx->uPacketCnt);
    }

    Log3Func(("%RU32 / %RU32 -> ioNumberDataPackets=%RU32\n",
              pConvCbCtx->uPacketIdx, pConvCbCtx->uPacketCnt, *ioNumberDataPackets));

    return noErr;
}
#endif /* VBOX_WITH_AUDIO_CA_CONVERTER */

#ifndef VBOX_WITH_AUDIO_CA_QUEUES
/**
 * Implements the OS X callback AURenderCallback in order to feed the audio
 * input buffer.
 */
static OSStatus coreAudioRecordingCb(void                       *pvUser,
                                     AudioUnitRenderActionFlags *pActionFlags,
                                     const AudioTimeStamp       *pAudioTS,
                                     UInt32                      uBusID,
                                     UInt32                      cFrames,
                                     AudioBufferList            *pBufData)
{
    RT_NOREF(pBufData);

    /* If nothing is pending return immediately. */
    if (cFrames == 0)
        return noErr;

    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pvUser;

    if (ASMAtomicReadU32(&pStreamIn->status) != CA_STATUS_INIT)
        return noErr;

    PCOREAUDIOCONVCBCTX pConvCbCtx = &pStreamIn->convCbCtx;

    OSStatus err = noErr;
    int rc = VINF_SUCCESS;

    Assert(pConvCbCtx->bufLstSrc.mNumberBuffers == 1);
    AudioBuffer *pSrcBuf = &pConvCbCtx->bufLstSrc.mBuffers[0];

    pConvCbCtx->uPacketCnt = cFrames / pConvCbCtx->asbdSrc.mFramesPerPacket;
    pConvCbCtx->uPacketIdx = 0;

    AudioConverterReset(pStreamIn->pConverter);

    Log3Func(("cFrames=%RU32 (%RU32 frames per packet) -> %RU32 packets\n",
               cFrames, pConvCbCtx->asbdSrc.mFramesPerPacket, pConvCbCtx->uPacketCnt));

    do
    {
#ifdef VBOX_WITH_AUDIO_CA_CONVERTER
        /* Are we using a converter? */
        if (pStreamIn->pConverter)
        {
            pSrcBuf->mNumberChannels = pConvCbCtx->asbdSrc.mChannelsPerFrame;
            pSrcBuf->mDataByteSize   = pConvCbCtx->asbdSrc.mBytesPerFrame * cFrames;
            pSrcBuf->mData           = RTMemAllocZ(pSrcBuf->mDataByteSize);
            if (!pSrcBuf->mData)
            {
                rc = VERR_NO_MEMORY;
                break;
            }

            /* First, render the source data as usual. */
            err = AudioUnitRender(pStreamIn->audioUnit, pActionFlags, pAudioTS, uBusID, cFrames, &pConvCbCtx->bufLstSrc);
            if (err != noErr)
            {
                LogRel2(("CoreAudio: Failed rendering converted audio input data (%RI32)\n", err));
                rc = VERR_IO_GEN_FAILURE; /** @todo Improve this. */
                break;
            }

            Log3Func(("cbSrcBufSize=%RU32\n", pSrcBuf->mDataByteSize));

#ifdef DEBUG_DUMP_PCM_DATA
            RTFILE fh;
            rc = RTFileOpen(&fh, DEBUG_DUMP_PCM_DATA_PATH "ca-recording-cb-src.pcm",
                            RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND | RTFILE_O_WRITE | RTFILE_O_DENY_NONE);
            if (RT_SUCCESS(rc))
            {
                RTFileWrite(fh, pSrcBuf->mData, pSrcBuf->mDataByteSize, NULL);
                RTFileClose(fh);
            }
            else
                AssertFailed();
#endif
            AudioBufferList dstBufList;

            dstBufList.mNumberBuffers = 1; /* We only use one buffer at once. */

            AudioBuffer *pDstBuf = &dstBufList.mBuffers[0];
            pDstBuf->mDataByteSize   = pConvCbCtx->asbdDst.mBytesPerFrame * cFrames;
            pDstBuf->mData           = RTMemAllocZ(pDstBuf->mDataByteSize);
            if (!pDstBuf->mData)
            {
                rc = VERR_NO_MEMORY;
                break;
            }

            UInt32 cPacketsToWriteAndWritten = pConvCbCtx->uPacketCnt;
            Assert(cPacketsToWriteAndWritten);

            Log3Func(("cPacketsToWrite=%RU32\n", cPacketsToWriteAndWritten));

            do
            {
                err = AudioConverterFillComplexBuffer(pStreamIn->pConverter,
                                                      coreAudioConverterCallback, pConvCbCtx /* pvData */,
                                                      &cPacketsToWriteAndWritten, &dstBufList, NULL);

                Log3Func(("cPacketsWritten=%RU32 (%zu bytes), err=%RI32\n",
                          cPacketsToWriteAndWritten, cPacketsToWriteAndWritten * pConvCbCtx->asbdDst.mBytesPerPacket, err));

                if (err != noErr)
                {
                    LogFlowFunc(("Failed to convert audio data (%RI32:%c%c%c%c)\n", err,
                                 RT_BYTE4(err), RT_BYTE3(err), RT_BYTE2(err), RT_BYTE1(err)));
                    rc = VERR_IO_GEN_FAILURE;
                    break;
                }

                if (cPacketsToWriteAndWritten == 0)
                    break;

                size_t cbDst = cPacketsToWriteAndWritten * pConvCbCtx->asbdDst.mBytesPerPacket;

#ifdef DEBUG_DUMP_PCM_DATA
                rc = RTFileOpen(&fh, DEBUG_DUMP_PCM_DATA_PATH "ca-recording-cb-dst.pcm",
                                RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND | RTFILE_O_WRITE | RTFILE_O_DENY_NONE);
                if (RT_SUCCESS(rc))
                {
                    RTFileWrite(fh, pDstBuf->mData, cbDst, NULL);
                    RTFileClose(fh);
                }
                else
                    AssertFailed();
#endif
                size_t cbFree = RTCircBufFree(pStreamIn->pCircBuf);
                if (cbFree < cbDst)
                {
                    LogRel2(("CoreAudio: Recording is lagging behind (%zu bytes available but only %zu bytes free)\n",
                             cbDst, cbFree));
                    break;
                }

                size_t cbDstChunk;
                void  *puDst;
                RTCircBufAcquireWriteBlock(pStreamIn->pCircBuf, cbDst, (void **)&puDst, &cbDstChunk);

                if (cbDstChunk)
                    memcpy(puDst, pDstBuf->mData, cbDstChunk);

                RTCircBufReleaseWriteBlock(pStreamIn->pCircBuf, cbDstChunk);

            } while (1);

            if (pDstBuf->mData)
            {
                RTMemFree(pDstBuf->mData);
                pDstBuf->mData = NULL;
            }
        }
        else /* No converter being used. */
        {
#endif /* VBOX_WITH_AUDIO_CA_CONVERTER */
            AssertBreakStmt(pStreamIn->streamFormat.mChannelsPerFrame >= 1, rc = VERR_INVALID_PARAMETER);
            AssertBreakStmt(pStreamIn->streamFormat.mBytesPerFrame >= 1,    rc = VERR_INVALID_PARAMETER);

            AssertBreakStmt(pSrcBuf->mNumberChannels, rc = VERR_INVALID_PARAMETER);

            pSrcBuf->mNumberChannels = pStreamIn->streamFormat.mChannelsPerFrame;
            pSrcBuf->mDataByteSize   = pStreamIn->streamFormat.mBytesPerFrame * cFrames;
            pSrcBuf->mData           = RTMemAlloc(pSrcBuf->mDataByteSize);
            if (!pSrcBuf->mData)
            {
                rc = VERR_NO_MEMORY;
                break;
            }

            err = AudioUnitRender(pStreamIn->audioUnit, pActionFlags, pAudioTS, uBusID, cFrames, &pConvCbCtx->bufLstSrc);
            if (err != noErr)
            {
                LogRel2(("CoreAudio: Failed rendering non-coverted audio input data (%RI32)\n", err));
                rc = VERR_IO_GEN_FAILURE; /** @todo Improve this. */
                break;
            }

            const uint32_t cbDataSize = pSrcBuf->mDataByteSize;
            const size_t   cbBufFree  = RTCircBufFree(pStreamIn->pCircBuf);
                  size_t   cbAvail    = RT_MIN(cbDataSize, cbBufFree);

            Log3Func(("cbDataSize=%RU32, cbBufFree=%zu, cbAvail=%zu\n", cbDataSize, cbBufFree, cbAvail));

            /* Iterate as long as data is available. */
            uint8_t *puDst = NULL;
            uint32_t cbWrittenTotal = 0;
            while (cbAvail)
            {
                /* Try to acquire the necessary space from the ring buffer. */
                size_t cbToWrite = 0;
                RTCircBufAcquireWriteBlock(pStreamIn->pCircBuf, cbAvail, (void **)&puDst, &cbToWrite);
                if (!cbToWrite)
                    break;

                /* Copy the data from the Core Audio buffer to the ring buffer. */
                memcpy(puDst, (uint8_t *)pSrcBuf->mData + cbWrittenTotal, cbToWrite);

                /* Release the ring buffer, so the main thread could start reading this data. */
                RTCircBufReleaseWriteBlock(pStreamIn->pCircBuf, cbToWrite);

                cbWrittenTotal += cbToWrite;

                Assert(cbAvail >= cbToWrite);
                cbAvail -= cbToWrite;
            }

            Log3Func(("cbWrittenTotal=%RU32, cbLeft=%zu\n", cbWrittenTotal, cbAvail));

#ifdef VBOX_WITH_AUDIO_CA_CONVERTER
        }
#endif /* VBOX_WITH_AUDIO_CA_CONVERTER */

    } while (0);

    if (pSrcBuf->mData)
    {
        RTMemFree(pSrcBuf->mData);
        pSrcBuf->mData = NULL;
    }

    return err;
}
#endif /* !VBOX_WITH_AUDIO_CA_QUEUES */

#define CA_BREAK_STMT(stmt) if (true) \
    { \
        stmt; \
        break; \
    } else do { } while (0)

#ifndef VBOX_WITH_AUDIO_CA_QUEUES
/** @todo Eventually split up this function, as this already is huge! */
static int coreAudioStreamInitIn(PCOREAUDIOSTREAMIN pStreamIn, PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    int rc = VINF_SUCCESS;

    UInt32 cSamples = 0;

    OSStatus err = noErr;
    UInt32 uSize = 0;

    AudioDeviceID deviceID = pStreamIn->deviceID;
    if (deviceID == kAudioDeviceUnknown)
    {
        /* Fetch the default audio recording device currently in use. */
        AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultInputDevice,
                                               kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
        uSize = sizeof(deviceID);
        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdr, 0, NULL, &uSize,  &deviceID);
        if (err != noErr)
        {
            LogFlowFunc(("CoreAudio: Unable to determine default recording device (%RI32)\n", err));
            return VERR_AUDIO_NO_FREE_INPUT_STREAMS;
        }
    }

    if (deviceID == kAudioDeviceUnknown)
    {
        LogFlowFunc(("No default recording device found\n"));
        return VERR_AUDIO_NO_FREE_INPUT_STREAMS;
    }

    do
    {
        /* Assign device ID. */
        pStreamIn->deviceID = deviceID;

        /*
         * Try to get the name of the recording device and log it. It's not fatal if it fails.
         */
        CFStringRef strTemp;

        AudioObjectPropertyAddress propAdr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMaster };
        uSize = sizeof(CFStringRef);
        err = AudioObjectGetPropertyData(pStreamIn->deviceID, &propAdr, 0, NULL, &uSize, &strTemp);
        if (err == noErr)
        {
            char *pszDevName = NULL;
            err = coreAudioCFStringToCString(strTemp, &pszDevName);
            if (err == noErr)
            {
                CFRelease(strTemp);

                /* Get the device' UUID. */
                propAdr.mSelector = kAudioDevicePropertyDeviceUID;
                err = AudioObjectGetPropertyData(pStreamIn->deviceID, &propAdr, 0, NULL, &uSize, &strTemp);
                if (err == noErr)
                {
                    char *pszUID = NULL;

                    err = coreAudioCFStringToCString(strTemp, &pszUID);
                    if (err == noErr)
                    {
                        CFRelease(strTemp);
                        LogRel(("CoreAudio: Using recording device: %s (UID: %s)\n", pszDevName, pszUID));

                        RTMemFree(pszUID);
                    }
                }

                RTMemFree(pszDevName);
            }
        }
        else
        {
            /* This is not fatal, can happen for some Macs. */
            LogRel2(("CoreAudio: Unable to determine recording device name (%RI32)\n", err));
        }

        /* Get the default frames buffer size, so that we can setup our internal buffers. */
        UInt32 cFrames;
        uSize = sizeof(cFrames);
        propAdr.mSelector = kAudioDevicePropertyBufferFrameSize;
        propAdr.mScope    = kAudioDevicePropertyScopeInput;
        err = AudioObjectGetPropertyData(pStreamIn->deviceID, &propAdr, 0, NULL, &uSize, &cFrames);
        if (err != noErr)
        {
            /* Can happen if no recording device is available by default. Happens on some Macs,
             * so don't log this by default to not scare people. */
            LogRel2(("CoreAudio: Failed to determine frame buffer size of the audio recording device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Set the frame buffer size and honor any minimum/maximum restrictions on the device. */
        err = coreAudioSetFrameBufferSize(pStreamIn->deviceID, true /* fInput */, cFrames, &cFrames);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set frame buffer size for the audio recording device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        LogFlowFunc(("cFrames=%RU32\n", cFrames));

        /* Try to find the default HAL output component. */
        AudioComponentDescription cd;

        RT_ZERO(cd);
        cd.componentType         = kAudioUnitType_Output;
        cd.componentSubType      = kAudioUnitSubType_HALOutput;
        cd.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent cp = AudioComponentFindNext(NULL, &cd);
        if (cp == 0)
        {
            LogRel(("CoreAudio: Failed to find HAL output component\n")); /** @todo Return error value? */
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Open the default HAL output component. */
        err = AudioComponentInstanceNew(cp, &pStreamIn->audioUnit);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to open output component (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Switch the I/O mode for input to on. */
        UInt32 uFlag = 1;
        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                   1, &uFlag, sizeof(uFlag));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to disable input I/O mode for input stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Switch the I/O mode for output to off. This is important, as this is a pure input stream. */
        uFlag = 0;
        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                   0, &uFlag, sizeof(uFlag));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to disable output I/O mode for input stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Set the default audio recording device as the device for the new AudioUnit. */
        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                                   0, &pStreamIn->deviceID, sizeof(pStreamIn->deviceID));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set current device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /*
         * CoreAudio will inform us on a second thread for new incoming audio data.
         * Therefore register a callback function which will process the new data.
         */
        AURenderCallbackStruct cb;
        RT_ZERO(cb);
        cb.inputProc       = coreAudioRecordingCb;
        cb.inputProcRefCon = pStreamIn;

        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
                                   0, &cb, sizeof(cb));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to register input callback (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Fetch the current stream format of the device. */
        RT_ZERO(pStreamIn->deviceFormat);
        uSize = sizeof(pStreamIn->deviceFormat);
        err = AudioUnitGetProperty(pStreamIn->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                   1, &pStreamIn->deviceFormat, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to get device format (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Create an AudioStreamBasicDescription based on our required audio settings. */
        RT_ZERO(pStreamIn->streamFormat);
        coreAudioStreamCfgToASBD(pCfgReq, &pStreamIn->streamFormat);

        coreAudioPrintASBD("Recording device", &pStreamIn->deviceFormat);
        coreAudioPrintASBD("Recording stream", &pStreamIn->streamFormat);

        /* If the frequency of the device is different from the requested one we
         * need a converter. The same count if the number of channels is different. */
        if (   pStreamIn->deviceFormat.mSampleRate       != pStreamIn->streamFormat.mSampleRate
            || pStreamIn->deviceFormat.mChannelsPerFrame != pStreamIn->streamFormat.mChannelsPerFrame)
        {
            LogRel2(("CoreAudio: Input converter is active\n"));

            err = AudioConverterNew(&pStreamIn->deviceFormat, &pStreamIn->streamFormat, &pStreamIn->pConverter);
            if (RT_UNLIKELY(err != noErr))
            {
                LogRel(("CoreAudio: Failed to create the audio converter (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }

            if (   pStreamIn->deviceFormat.mChannelsPerFrame == 1 /* Mono */
                && pStreamIn->streamFormat.mChannelsPerFrame == 2 /* Stereo */)
            {
                LogRel2(("CoreAudio: Mono to stereo conversion active\n"));

                /*
                 * If the channel count is different we have to tell this the converter
                 * and supply a channel mapping. For now we only support mapping
                 * from mono to stereo. For all other cases the core audio defaults
                 * are used, which means dropping additional channels in most
                 * cases.
                 */
                const SInt32 channelMap[2] = {0, 0}; /* Channel map for mono -> stereo. */

                err = AudioConverterSetProperty(pStreamIn->pConverter, kAudioConverterChannelMap, sizeof(channelMap), channelMap);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to set channel mapping (mono -> stereo) for the audio input converter (%RI32)\n", err));
                    CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
                }
            }

            /* Set sample rate converter quality to maximum. */
            uFlag = kAudioConverterQuality_Max;
            err = AudioConverterSetProperty(pStreamIn->pConverter, kAudioConverterSampleRateConverterQuality,
                                            sizeof(uFlag), &uFlag);
            if (err != noErr)
                LogRel2(("CoreAudio: Failed to set input audio converter quality to the maximum (%RI32)\n", err));

            uSize = sizeof(UInt32);
            UInt32 maxOutputSize;
            err = AudioConverterGetProperty(pStreamIn->pConverter, kAudioConverterPropertyMaximumOutputPacketSize,
                                            &uSize, &maxOutputSize);
            if (RT_UNLIKELY(err != noErr))
            {
                LogRel(("CoreAudio: Failed to retrieve converter's maximum output size (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }

            LogFunc(("Maximum converter packet output size is: %RI32\n", maxOutputSize));

            /* Set the input (source) format, that is, the format the device is recording data with. */
            err = AudioUnitSetProperty(pStreamIn->audioUnit,
                                       kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Input,
                                       1,
                                       &pStreamIn->deviceFormat,
                                       sizeof(pStreamIn->deviceFormat));
            if (RT_UNLIKELY(err != noErr))
            {
                LogRel(("CoreAudio: Failed to set device input format (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }

            /* Set the output (target) format, that is, the format we created the input stream with. */
            err = AudioUnitSetProperty(pStreamIn->audioUnit,
                                       kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Output,
                                       1,
                                       &pStreamIn->deviceFormat,
                                       sizeof(pStreamIn->deviceFormat));
            if (RT_UNLIKELY(err != noErr))
            {
                LogRel(("CoreAudio: Failed to set stream output format (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }
        }
        else
        {
            /* Set the new output format description for the input stream. */
            err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                       1, &pStreamIn->streamFormat, sizeof(pStreamIn->streamFormat));
            if (err != noErr)
            {
                LogRel(("CoreAudio: Failed to set output format for input stream (%RI32)\n", err));
                CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
            }
        }

        /*
         * Also set the frame buffer size off the device on our AudioUnit. This
         * should make sure that the frames count which we receive in the render
         * thread is as we like.
         */
        err = AudioUnitSetProperty(pStreamIn->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                                   1, &cFrames, sizeof(cFrames));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set maximum frame buffer size for input stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Finally initialize the new AudioUnit. */
        err = AudioUnitInitialize(pStreamIn->audioUnit);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to initialize audio unit for input stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        uSize = sizeof(pStreamIn->deviceFormat);
        err = AudioUnitGetProperty(pStreamIn->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                   1, &pStreamIn->deviceFormat, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to get recording device format (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /*
         * There are buggy devices (e.g. my Bluetooth headset) which doesn't honor
         * the frame buffer size set in the previous calls. So finally get the
         * frame buffer size after the AudioUnit was initialized.
         */
        uSize = sizeof(cFrames);
        err = AudioUnitGetProperty(pStreamIn->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                                   0, &cFrames, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to get maximum frame buffer size from input audio device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Destroy any former internal ring buffer. */
        if (pStreamIn->pCircBuf)
        {
            RTCircBufDestroy(pStreamIn->pCircBuf);
            pStreamIn->pCircBuf = NULL;
        }

#ifdef VBOX_WITH_AUDIO_CA_CONVERTER
        coreAudioUninitConvCbCtx(&pStreamIn->convCbCtx);
#endif
        /* Calculate the ratio between the device and the stream sample rate. */
        pStreamIn->sampleRatio = pStreamIn->streamFormat.mSampleRate / pStreamIn->deviceFormat.mSampleRate;

        /*
         * Make sure that the ring buffer is big enough to hold the recording
         * data. Compare the maximum frames per slice value with the frames
         * necessary when using the converter where the sample rate could differ.
         * The result is always multiplied by the channels per frame to get the
         * samples count.
         */
        cSamples = RT_MAX(cFrames,
                          (cFrames * pStreamIn->deviceFormat.mBytesPerFrame * pStreamIn->sampleRatio)
                           / pStreamIn->streamFormat.mBytesPerFrame)
                           * pStreamIn->streamFormat.mChannelsPerFrame;
        if (!cSamples)
        {
            LogRel(("CoreAudio: Failed to determine samples buffer count input stream\n"));
            CA_BREAK_STMT(rc = VERR_INVALID_PARAMETER);
        }

        PDMAUDIOPCMPROPS PCMProps;
        int rc2 = DrvAudioHlpStreamCfgToProps(pCfgAcq, &PCMProps);
        AssertRC(rc2);

        rc = RTCircBufCreate(&pStreamIn->pCircBuf, cSamples << PCMProps.cShift);
        if (RT_FAILURE(rc))
            break;

#ifdef VBOX_WITH_AUDIO_CA_CONVERTER
        /* Init the converter callback context. */
        rc = coreAudioInitConvCbCtx(&pStreamIn->convCbCtx,
                                    &pStreamIn->deviceFormat /* Source */, &pStreamIn->streamFormat /* Dest */);
#endif
        if (RT_SUCCESS(rc))
        {
#ifdef DEBUG
            propAdr.mSelector = kAudioDeviceProcessorOverload;
            propAdr.mScope    = kAudioUnitScope_Global;
            err = AudioObjectAddPropertyListener(pStreamIn->deviceID, &propAdr,
                                                 coreAudioRecordingAudioDevicePropertyChanged, (void *)pStreamIn);
            if (RT_UNLIKELY(err != noErr))
                LogRel2(("CoreAudio: Failed to add the processor overload listener for input stream (%RI32)\n", err));
#endif /* DEBUG */
            propAdr.mSelector = kAudioDevicePropertyNominalSampleRate;
            propAdr.mScope    = kAudioUnitScope_Global;
            err = AudioObjectAddPropertyListener(pStreamIn->deviceID, &propAdr,
                                                 coreAudioRecordingAudioDevicePropertyChanged, (void *)pStreamIn);
            /* Not fatal. */
            if (RT_UNLIKELY(err != noErr))
                LogRel2(("CoreAudio: Failed to register sample rate changed listener for input stream (%RI32)\n", err));
        }

    } while (0);

    if (RT_SUCCESS(rc))
    {
        LogFunc(("cSamples=%RU32\n", cSamples));

        if (pCfgAcq)
            pCfgAcq->cSampleBufferSize = cSamples;
    }
    else
    {
        AudioUnitUninitialize(pStreamIn->audioUnit);

        if (pStreamIn->pCircBuf)
        {
            RTCircBufDestroy(pStreamIn->pCircBuf);
            pStreamIn->pCircBuf = NULL;
        }

#ifdef VBOX_WITH_AUDIO_CA_CONVERTER
        coreAudioUninitConvCbCtx(&pStreamIn->convCbCtx);
#endif
    }

    LogFunc(("rc=%Rrc\n", rc));
    return rc;
}

/** @todo Eventually split up this function, as this already is huge! */
static int coreAudioStreamInitOut(PCOREAUDIOSTREAMOUT pStreamOut, PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    int rc = VINF_SUCCESS;

    UInt32 cSamples = 0;

    OSStatus err = noErr;
    UInt32 uSize = 0;

    AudioDeviceID deviceID = pStreamOut->deviceID;
    if (deviceID == kAudioDeviceUnknown)
    {
        /* Fetch the default audio recording device currently in use. */
        AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultOutputDevice,
                                               kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
        uSize = sizeof(deviceID);
        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAdr, 0, NULL, &uSize, &deviceID);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Unable to determine default playback device (%RI32)\n", err));
            return VERR_NOT_FOUND;
        }
    }

    if (deviceID == kAudioDeviceUnknown)
    {
        LogFlowFunc(("No default playback device found\n"));
        return VERR_NOT_FOUND;
    }

    do
    {
        /* Assign device ID. */
        pStreamOut->deviceID = deviceID;

        /*
         * Try to get the name of the playback device and log it. It's not fatal if it fails.
         */
        CFStringRef strTemp;

        AudioObjectPropertyAddress propAdr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMaster };
        uSize = sizeof(CFStringRef);
        err = AudioObjectGetPropertyData(pStreamOut->deviceID, &propAdr, 0, NULL, &uSize, &strTemp);
        if (err == noErr)
        {
            char *pszDevName = NULL;
            err = coreAudioCFStringToCString(strTemp, &pszDevName);
            if (err == noErr)
            {
                CFRelease(strTemp);

                /* Get the device' UUID. */
                propAdr.mSelector = kAudioDevicePropertyDeviceUID;
                err = AudioObjectGetPropertyData(pStreamOut->deviceID, &propAdr, 0, NULL, &uSize, &strTemp);
                if (err == noErr)
                {
                    char *pszUID = NULL;
                    err = coreAudioCFStringToCString(strTemp, &pszUID);
                    if (err == noErr)
                    {
                        CFRelease(strTemp);
                        LogRel(("CoreAudio: Using playback device: %s (UID: %s)\n", pszDevName, pszUID));

                        RTMemFree(pszUID);
                    }
                }

                RTMemFree(pszDevName);
            }
        }
        else
            LogRel(("CoreAudio: Unable to determine playback device name (%RI32)\n", err));

        /* Get the default frames buffer size, so that we can setup our internal buffers. */
        UInt32 cFrames;
        uSize = sizeof(cFrames);
        propAdr.mSelector = kAudioDevicePropertyBufferFrameSize;
        propAdr.mScope    = kAudioDevicePropertyScopeInput;
        err = AudioObjectGetPropertyData(pStreamOut->deviceID, &propAdr, 0, NULL, &uSize, &cFrames);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to determine frame buffer size of the audio playback device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Set the frame buffer size and honor any minimum/maximum restrictions on the device. */
        err = coreAudioSetFrameBufferSize(pStreamOut->deviceID, false /* fInput */, cFrames, &cFrames);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set frame buffer size for the audio playback device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Try to find the default HAL output component. */
        AudioComponentDescription cd;
        RT_ZERO(cd);

        cd.componentType         = kAudioUnitType_Output;
        cd.componentSubType      = kAudioUnitSubType_HALOutput;
        cd.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent cp = AudioComponentFindNext(NULL, &cd);
        if (cp == 0)
        {
            LogRel(("CoreAudio: Failed to find HAL output component\n")); /** @todo Return error value? */
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Open the default HAL output component. */
        err = AudioComponentInstanceNew(cp, &pStreamOut->audioUnit);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to open output component (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Switch the I/O mode for output to on. */
        UInt32 uFlag = 1;
        err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                   0, &uFlag, sizeof(uFlag));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to disable I/O mode for output stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Set the default audio playback device as the device for the new AudioUnit. */
        err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                                   0, &pStreamOut->deviceID, sizeof(pStreamOut->deviceID));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set current device for output stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /*
         * CoreAudio will inform us on a second thread for new incoming audio data.
         * Therefor register a callback function which will process the new data.
         */
        AURenderCallbackStruct cb;
        RT_ZERO(cb);
        cb.inputProc       = coreAudioPlaybackCb; /* pvUser */
        cb.inputProcRefCon = pStreamOut;

        err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                   0, &cb, sizeof(cb));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to register output callback (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Fetch the current stream format of the device. */
        uSize = sizeof(pStreamOut->deviceFormat);
        err = AudioUnitGetProperty(pStreamOut->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                   0, &pStreamOut->deviceFormat, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to get device format (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Create an AudioStreamBasicDescription based on our required audio settings. */
        coreAudioStreamCfgToASBD(pCfgReq, &pStreamOut->streamFormat);

        coreAudioPrintASBD("Playback device", &pStreamOut->deviceFormat);
        coreAudioPrintASBD("Playback format", &pStreamOut->streamFormat);

        /* Set the new output format description for the stream. */
        err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                   0, &pStreamOut->streamFormat, sizeof(pStreamOut->streamFormat));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set stream format for output stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        uSize = sizeof(pStreamOut->deviceFormat);
        err = AudioUnitGetProperty(pStreamOut->audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                   0, &pStreamOut->deviceFormat, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to retrieve device format for output stream (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /*
         * Also set the frame buffer size off the device on our AudioUnit. This
         * should make sure that the frames count which we receive in the render
         * thread is as we like.
         */
        err = AudioUnitSetProperty(pStreamOut->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                                   0, &cFrames, sizeof(cFrames));
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to set maximum frame buffer size for output AudioUnit (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /* Finally initialize the new AudioUnit. */
        err = AudioUnitInitialize(pStreamOut->audioUnit);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to initialize the output audio device (%RI32)\n", err));
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /*
         * There are buggy devices (e.g. my Bluetooth headset) which doesn't honor
         * the frame buffer size set in the previous calls. So finally get the
         * frame buffer size after the AudioUnit was initialized.
         */
        uSize = sizeof(cFrames);
        err = AudioUnitGetProperty(pStreamOut->audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                                   0, &cFrames, &uSize);
        if (err != noErr)
        {
            LogRel(("CoreAudio: Failed to get maximum frame buffer size from output audio device (%RI32)\n", err));

            AudioUnitUninitialize(pStreamOut->audioUnit);
            CA_BREAK_STMT(rc = VERR_AUDIO_BACKEND_INIT_FAILED);
        }

        /*
         * Make sure that the ring buffer is big enough to hold the recording
         * data. Compare the maximum frames per slice value with the frames
         * necessary when using the converter where the sample rate could differ.
         * The result is always multiplied by the channels per frame to get the
         * samples count.
         */
        cSamples = cFrames * pStreamOut->streamFormat.mChannelsPerFrame;
        if (!cSamples)
        {
            LogRel(("CoreAudio: Failed to determine samples buffer count output stream\n"));
            CA_BREAK_STMT(rc = VERR_INVALID_PARAMETER);
        }

        /* Destroy any former internal ring buffer. */
        if (pStreamOut->pCircBuf)
        {
            RTCircBufDestroy(pStreamOut->pCircBuf);
            pStreamOut->pCircBuf = NULL;
        }

        PDMAUDIOPCMPROPS PCMProps;
        int rc2 = DrvAudioHlpStreamCfgToProps(pCfgAcq, &PCMProps);
        AssertRC(rc2);

        rc = RTCircBufCreate(&pStreamOut->pCircBuf, cSamples << PCMProps.cShift);
        if (RT_FAILURE(rc))
            break;

        /*
         * Register callbacks.
         */
#ifdef DEBUG
        propAdr.mSelector = kAudioDeviceProcessorOverload;
        propAdr.mScope    = kAudioUnitScope_Global;
        err = AudioObjectAddPropertyListener(pStreamOut->deviceID, &propAdr,
                                             coreAudioPlaybackAudioDevicePropertyChanged, (void *)pStreamOut);
        if (err != noErr)
            LogRel(("CoreAudio: Failed to register processor overload listener for output stream (%RI32)\n", err));
#endif /* DEBUG */

        propAdr.mSelector = kAudioDevicePropertyNominalSampleRate;
        propAdr.mScope    = kAudioUnitScope_Global;
        err = AudioObjectAddPropertyListener(pStreamOut->deviceID, &propAdr,
                                             coreAudioPlaybackAudioDevicePropertyChanged, (void *)pStreamOut);
        /* Not fatal. */
        if (err != noErr)
            LogRel(("CoreAudio: Failed to register sample rate changed listener for output stream (%RI32)\n", err));

    } while (0);

    if (RT_SUCCESS(rc))
    {
        LogFunc(("cSamples=%RU32\n", cSamples));

        if (pCfgAcq)
            pCfgAcq->cSampleBufferSize = cSamples;
    }
    else
    {
        AudioUnitUninitialize(pStreamOut->audioUnit);

        if (pStreamOut->pCircBuf)
        {
            RTCircBufDestroy(pStreamOut->pCircBuf);
            pStreamOut->pCircBuf = NULL;
        }
    }

    LogFunc(("cSamples=%RU32, rc=%Rrc\n", cSamples, rc));
    return rc;
}
#endif /* VBOX_WITH_AUDIO_CA_QUEUES */

/**
 * Implements the OS X callback AudioObjectPropertyListenerProc for getting
 * notified when some of the properties of an audio device has changed.
 */
static OSStatus coreAudioPlaybackAudioDevicePropertyChanged(AudioObjectID propertyID,
                                                            UInt32 cAddresses,
                                                            const AudioObjectPropertyAddress paProperties[],
                                                            void *pvUser)
{
    RT_NOREF(propertyID, cAddresses, paProperties, pvUser)

    switch (propertyID)
    {
#ifdef DEBUG
#endif /* DEBUG */
        default:
            break;
    }

    return noErr;
}

#ifndef VBOX_WITH_AUDIO_CA_QUEUES
/**
 * Implements the OS X callback AURenderCallback in order to feed the audio
 * output buffer.
 */
static OSStatus coreAudioPlaybackCb(void *pvUser,
                                    AudioUnitRenderActionFlags *pActionFlags,
                                    const AudioTimeStamp       *pAudioTS,
                                    UInt32                      uBusID,
                                    UInt32                      cFrames,
                                    AudioBufferList            *pBufData)
{
    RT_NOREF(pActionFlags, pAudioTS, uBusID, cFrames);
    PCOREAUDIOSTREAMOUT pStreamOut  = (PCOREAUDIOSTREAMOUT)pvUser;

    if (ASMAtomicReadU32(&pStreamOut->status) != CA_STATUS_INIT)
    {
        pBufData->mBuffers[0].mDataByteSize = 0;
        return noErr;
    }

    /* How much space is used in the ring buffer? */
    size_t cbToRead = RT_MIN(RTCircBufUsed(pStreamOut->pCircBuf), pBufData->mBuffers[0].mDataByteSize);
    if (!cbToRead)
    {
        pBufData->mBuffers[0].mDataByteSize = 0;
        return noErr;
    }

    uint8_t *pbSrc = NULL;
    size_t cbRead  = 0;

    size_t cbLeft  = cbToRead;
    while (cbLeft)
    {
        /* Try to acquire the necessary block from the ring buffer. */
        RTCircBufAcquireReadBlock(pStreamOut->pCircBuf, cbLeft, (void **)&pbSrc, &cbToRead);

        /* Break if nothing is used anymore. */
        if (!cbToRead)
            break;

        /* Copy the data from our ring buffer to the core audio buffer. */
        memcpy((uint8_t *)pBufData->mBuffers[0].mData + cbRead, pbSrc, cbToRead);

        /* Release the read buffer, so it could be used for new data. */
        RTCircBufReleaseReadBlock(pStreamOut->pCircBuf, cbToRead);

        /* Move offset. */
        cbRead += cbToRead;

        /* Check if we're lagging behind. */
        if (cbRead > pBufData->mBuffers[0].mDataByteSize)
        {
            LogRel2(("CoreAudio: Host output lagging behind, expect stuttering guest audio output\n"));
            cbRead = pBufData->mBuffers[0].mDataByteSize;
            break;
        }

        Assert(cbToRead <= cbLeft);
        cbLeft -= cbToRead;
    }

    /* Write the bytes to the core audio buffer which were really written. */
    Assert(pBufData->mBuffers[0].mDataByteSize >= cbRead);
    pBufData->mBuffers[0].mDataByteSize = cbRead;

    Log3Func(("Read %zu / %zu bytes\n", cbRead, cbToRead));

    return noErr;
}
#endif /* VBOX_WITH_AUDIO_CA_QUEUES */

/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnInit}
 *
 * @todo Please put me next to the shutdown function, because then it would be
 *       clear why I'm empty.  While at it, it would be nice if you also
 *       reordered my PDMIHOSTAUDIO sibilings according to the interface
 *       (doesn't matter if it's reversed (like all other PDM drivers and
 *       devices) or not).
 */
static DECLCALLBACK(int) drvHostCoreAudioInit(PPDMIHOSTAUDIO pInterface)
{
    RT_NOREF(pInterface);

    LogFlowFuncEnter();

    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnStreamCapture}
 */
static DECLCALLBACK(int) drvHostCoreAudioStreamCapture(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream,
                                                       void *pvBuf, uint32_t cbBuf, uint32_t *pcbRead)
{
    RT_NOREF(pvBuf, cbBuf); /** @todo r=bird: this looks totally weird at first glance! */

    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);
    /* pcbRead is optional. */

    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;
    PCOREAUDIOSTREAM   pCAStream = &pStreamIn->Common;

    /* Check if the audio device should be reinitialized. If so do it. */
    if (ASMAtomicReadU32(&pStreamIn->status) == CA_STATUS_REINIT)
    {
        int rc2 = coreAudioStreamReinitIn(pCAStream);
        if (RT_FAILURE(rc2))
            return VERR_NOT_AVAILABLE;
    }

    if (ASMAtomicReadU32(&pStreamIn->status) != CA_STATUS_INIT)
    {
        if (pcbRead)
            *pcbRead = 0;
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;
    uint32_t cbWrittenTotal = 0;

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    rc = RTCritSectEnter(&pCAStream->CritSect);
    AssertRC(rc);
#endif

    PRTCIRCBUF pCircBuf = NULL;
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    pCircBuf = pCAStream->pCircBuf;
#else
    pCircBuf = pStreamIn->pCircBuf;
#endif
    AssertPtr(pCircBuf);

    do
    {
        size_t cbMixBuf  = AudioMixBufSizeBytes(&pStream->MixBuf);
        size_t cbToWrite = RT_MIN(cbMixBuf, RTCircBufUsed(pCircBuf));

        uint32_t cWritten, cbWritten;

        uint8_t *pvChunk;
        size_t   cbChunk;

        Log3Func(("cbMixBuf=%zu, cbToWrite=%zu/%zu\n", cbMixBuf, cbToWrite, RTCircBufSize(pCircBuf)));

        while (cbToWrite)
        {
            /* Try to acquire the necessary block from the ring buffer. */
            RTCircBufAcquireReadBlock(pCircBuf, cbToWrite, (void **)&pvChunk, &cbChunk);
            if (cbChunk)
            {
#ifdef DEBUG_DUMP_PCM_DATA
                RTFILE fh;
                rc = RTFileOpen(&fh, DEBUG_DUMP_PCM_DATA_PATH "ca-capture.pcm",
                                RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND | RTFILE_O_WRITE | RTFILE_O_DENY_NONE);
                if (RT_SUCCESS(rc))
                {
                    RTFileWrite(fh, pvChunk, cbChunk, NULL);
                    RTFileClose(fh);
                }
                else
                    AssertFailed();
#endif
                rc = AudioMixBufWriteCirc(&pStream->MixBuf, pvChunk, cbChunk, &cWritten);
                if (rc == VERR_BUFFER_OVERFLOW)
                {
                    LogRel2(("Core Audio: Capturing host buffer full\n"));
                    rc = VINF_SUCCESS;
                }
            }

            /* Release the read buffer, so it could be used for new data. */
            RTCircBufReleaseReadBlock(pCircBuf, cbChunk);

            if (RT_FAILURE(rc))
                break;

            cbWritten = AUDIOMIXBUF_S2B(&pStream->MixBuf, cWritten);

            Assert(cbToWrite >= cbWritten);
            cbToWrite      -= cbWritten;
            cbWrittenTotal += cbWritten;
        }

        Log3Func(("cbToWrite=%zu, cbToRead=%zu, cbWrittenTotal=%RU32, rc=%Rrc\n", cbToWrite, cbChunk, cbWrittenTotal, rc));
    }
    while (0);

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    int rc2 = RTCritSectLeave(&pCAStream->CritSect);
    AssertRC(rc2);
#endif

    if (RT_SUCCESS(rc))
    {
        uint32_t cCaptured     = 0;
        uint32_t cWrittenTotal = AUDIOMIXBUF_B2S(&pStream->MixBuf, cbWrittenTotal);
        if (cWrittenTotal)
            rc = AudioMixBufMixToParent(&pStream->MixBuf, cWrittenTotal, &cCaptured);

        Log3Func(("cWrittenTotal=%RU32 (%RU32 bytes), cCaptured=%RU32, rc=%Rrc\n", cWrittenTotal, cbWrittenTotal, cCaptured, rc));

        if (cCaptured)
            LogFlowFunc(("%RU32 samples captured\n", cCaptured));

        if (pcbRead)
            *pcbRead = cCaptured;
    }

    if (RT_FAILURE(rc))
        LogFunc(("Failed with rc=%Rrc\n", rc));

    return rc;
}

/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnStreamPlay}
 */
static DECLCALLBACK(int) drvHostCoreAudioStreamPlay(PPDMIHOSTAUDIO pInterface,
                                                    PPDMAUDIOSTREAM pStream,
                                                    const void *pvBuf, uint32_t cbBuf, uint32_t *pcbWritten)
{
    RT_NOREF(pvBuf, cbBuf); /** @todo r=bird: this looks weird at first glance... */

    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);
    /* pcbWritten is optional. */

    PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;
    PCOREAUDIOSTREAM    pCAStream  = &pStreamOut->Common;

    /* Check if the audio device should be reinitialized. If so do it. */
    if (ASMAtomicReadU32(&pStreamOut->status) == CA_STATUS_REINIT)
    {
        /* For now re just re-initialize with the current output device. */
        int rc2 = coreAudioStreamReinitOut(pCAStream);
        if (RT_FAILURE(rc2))
            return VERR_NOT_AVAILABLE;
    }

    if (ASMAtomicReadU32(&pStreamOut->status) != CA_STATUS_INIT)
    {
        if (pcbWritten)
            *pcbWritten = 0;
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;

    uint32_t cLive = AudioMixBufLive(&pStream->MixBuf);
    if (!cLive) /* Not live samples to play? Bail out. */
    {
        if (pcbWritten)
            *pcbWritten = 0;
        return VINF_SUCCESS;
    }

    size_t cbLive  = AUDIOMIXBUF_S2B(&pStream->MixBuf, cLive);

    uint32_t cbReadTotal = 0;

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    rc = RTCritSectEnter(&pCAStream->CritSect);
    AssertRC(rc);
#endif

    PRTCIRCBUF pCircBuf = NULL;
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    pCircBuf = pCAStream->pCircBuf;
#else
    pCircBuf = pStreamOut->pCircBuf;
#endif
    AssertPtr(pCircBuf);

    size_t cbToRead = RT_MIN(cbLive, RTCircBufFree(pCircBuf));
    Log3Func(("cbToRead=%zu\n", cbToRead));

    uint8_t *pvChunk;
    size_t   cbChunk;

    while (cbToRead)
    {
        uint32_t cRead, cbRead;

        /* Try to acquire the necessary space from the ring buffer. */
        RTCircBufAcquireWriteBlock(pCircBuf, cbToRead, (void **)&pvChunk, &cbChunk);
        if (!cbChunk)
        {
            RTCircBufReleaseWriteBlock(pCircBuf, cbChunk);
            break;
        }

        Assert(cbChunk <= cbToRead);

        rc = AudioMixBufReadCirc(&pStream->MixBuf, pvChunk, cbChunk, &cRead);

        cbRead = AUDIOMIXBUF_S2B(&pStream->MixBuf, cRead);

        /* Release the ring buffer, so the read thread could start reading this data. */
        RTCircBufReleaseWriteBlock(pCircBuf, cbChunk);

        if (RT_FAILURE(rc))
            break;

        Assert(cbToRead >= cbRead);
        cbToRead -= cbRead;
        cbReadTotal += cbRead;
    }

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    if (    RT_SUCCESS(rc)
        &&  pCAStream->fRun
        && !pCAStream->fIsRunning)
    {
        rc = coreAudioStreamInvalidateQueue(pCAStream);
        if (RT_SUCCESS(rc))
        {
            AudioQueueStart(pCAStream->audioQueue, NULL);
            pCAStream->fRun       = false;
            pCAStream->fIsRunning = true;
        }
    }

    int rc2 = RTCritSectLeave(&pCAStream->CritSect);
    AssertRC(rc2);
#endif

    if (RT_SUCCESS(rc))
    {
        uint32_t cReadTotal = AUDIOMIXBUF_B2S(&pStream->MixBuf, cbReadTotal);
        if (cReadTotal)
            AudioMixBufFinish(&pStream->MixBuf, cReadTotal);

        Log3Func(("cReadTotal=%RU32 (%RU32 bytes)\n", cReadTotal, cbReadTotal));

        if (pcbWritten)
            *pcbWritten = cReadTotal;
    }

    return rc;
}

static int coreAudioControlStreamOut(PCOREAUDIOSTREAMOUT pCAStreamOut, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    LogFlowFunc(("enmStreamCmd=%RU32\n", enmStreamCmd));

    uint32_t uStatus = ASMAtomicReadU32(&pCAStreamOut->status);
    if (!(   uStatus == CA_STATUS_INIT
          || uStatus == CA_STATUS_REINIT))
    {
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    PCOREAUDIOSTREAM pCAStream = &pCAStreamOut->Common;
#endif

    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        case PDMAUDIOSTREAMCMD_RESUME:
        {
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
            LogFunc(("Queue enable\n"));
            if (pCAStream->enmDir == PDMAUDIODIR_IN)
            {
                rc = coreAudioStreamInvalidateQueue(pCAStream);
                if (RT_SUCCESS(rc))
                {
                    /* Start the audio queue immediately. */
                    AudioQueueStart(pCAStream->audioQueue, NULL);
                }
            }
            if (pCAStream->enmDir == PDMAUDIODIR_OUT)
            {
                /* Touch the run flag to start the audio queue as soon as
                 * we have anough data to actually play something. */
                ASMAtomicXchgBool(&pCAStream->fRun, true);
            }
#else
            /* Only start the device if it is actually stopped */
            if (!coreAudioIsRunning(pCAStreamOut->deviceID))
            {
                OSStatus err = AudioUnitReset(pCAStreamOut->audioUnit, kAudioUnitScope_Input, 0);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to reset AudioUnit (%RI32)\n", err));
                    /* Keep going. */
                }
                RTCircBufReset(pCAStreamOut->pCircBuf);

                err = AudioOutputUnitStart(pCAStreamOut->audioUnit);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to start playback (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                }
            }
#endif /* VBOX_WITH_AUDIO_CA_QUEUES */
            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
        {
            LogFunc(("Queue disable\n"));
            AudioQueueStop(pCAStream->audioQueue, 1 /* Immediately */);
            ASMAtomicXchgBool(&pCAStream->fRun,       false);
            ASMAtomicXchgBool(&pCAStream->fIsRunning, false);
            break;
        }
#endif
        case PDMAUDIOSTREAMCMD_PAUSE:
        {
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
            LogFunc(("Queue pause\n"));
            AudioQueuePause(pCAStream->audioQueue);
            ASMAtomicXchgBool(&pCAStream->fIsRunning, false);
#else
            /* Only stop the device if it is actually running */
            if (coreAudioIsRunning(pCAStreamOut->deviceID))
            {
                OSStatus err = AudioOutputUnitStop(pCAStreamOut->audioUnit);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to stop playback (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                    break;
                }

                err = AudioUnitReset(pCAStreamOut->audioUnit, kAudioUnitScope_Input, 0);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to reset AudioUnit (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                }
            }
#endif /* VBOX_WITH_AUDIO_CA_QUEUES */
            break;
        }

        default:
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioControlStreamIn(PCOREAUDIOSTREAMIN pCAStreamIn, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    LogFlowFunc(("enmStreamCmd=%RU32\n", enmStreamCmd));

    uint32_t uStatus = ASMAtomicReadU32(&pCAStreamIn->status);
    if (!(   uStatus == CA_STATUS_INIT
          || uStatus == CA_STATUS_REINIT))
    {
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    PCOREAUDIOSTREAM pCAStream = &pCAStreamIn->Common;
#endif

    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        case PDMAUDIOSTREAMCMD_RESUME:
        {
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
            LogFunc(("Queue enable\n"));
            if (pCAStream->enmDir == PDMAUDIODIR_IN)
            {
                rc = coreAudioStreamInvalidateQueue(pCAStream);
                if (RT_SUCCESS(rc))
                {
                    /* Start the audio queue immediately. */
                    AudioQueueStart(pCAStream->audioQueue, NULL);
                }
            }
            if (pCAStream->enmDir == PDMAUDIODIR_OUT)
            {
                /* Touch the run flag to start the audio queue as soon as
                 * we have anough data to actually play something. */
                ASMAtomicXchgBool(&pCAStream->fRun, true);
            }
#else
            /* Only start the device if it is actually stopped */
            if (!coreAudioIsRunning(pCAStreamIn->deviceID))
            {
                RTCircBufReset(pCAStreamIn->pCircBuf);
                OSStatus err = AudioOutputUnitStart(pCAStreamIn->audioUnit);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to start recording (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                    break;
                }
            }
#endif /* VBOX_WITH_AUDIO_CA_QUEUES */
            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
        {
            LogFunc(("Queue disable\n"));
            AudioQueueStop(pCAStream->audioQueue, 1 /* Immediately */);
            ASMAtomicXchgBool(&pCAStream->fRun,       false);
            ASMAtomicXchgBool(&pCAStream->fIsRunning, false);
            break;
        }
#endif
        case PDMAUDIOSTREAMCMD_PAUSE:
        {
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
            LogFunc(("Queue pause\n"));
            AudioQueuePause(pCAStream->audioQueue);
            ASMAtomicXchgBool(&pCAStream->fIsRunning, false);
#else
            /* Only stop the device if it is actually running */
            if (coreAudioIsRunning(pCAStreamIn->deviceID))
            {
                OSStatus err = AudioOutputUnitStop(pCAStreamIn->audioUnit);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to stop recording (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                    break;
                }

                err = AudioUnitReset(pCAStreamIn->audioUnit, kAudioUnitScope_Input, 0);
                if (err != noErr)
                {
                    LogRel(("CoreAudio: Failed to reset AudioUnit (%RI32)\n", err));
                    rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
                    break;
                }
            }
#endif /* VBOX_WITH_AUDIO_CA_QUEUES */
            break;
        }

        default:
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioDestroyStreamIn(PPDMAUDIOSTREAM pStream)
{
    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;
    PCOREAUDIOSTREAM   pCAStream = &pStreamIn->Common;

    LogFlowFuncEnter();

    uint32_t status = ASMAtomicReadU32(&pStreamIn->status);
    if (!(   status == CA_STATUS_INIT
          || status == CA_STATUS_REINIT))
    {
        return VINF_SUCCESS;
    }

    OSStatus err = noErr;

    int rc = coreAudioControlStreamIn(pStreamIn, PDMAUDIOSTREAMCMD_DISABLE);
    if (RT_SUCCESS(rc))
    {
        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_IN_UNINIT);

        rc = coreAudioStreamUninit(pCAStream);
        if (RT_SUCCESS(rc))
        {
            ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_UNINIT);

            /*
             * Unregister recording device callbacks.
             */
            AudioObjectPropertyAddress propAdr = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMaster };
#ifdef DEBUG
            err = AudioObjectRemovePropertyListener(pStreamIn->deviceID, &propAdr,
                                                    coreAudioRecordingAudioDevicePropertyChanged, &pStreamIn->Common);
            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to remove the recording processor overload listener (%RI32)\n", err));
            }
#endif /* DEBUG */

            propAdr.mSelector = kAudioDevicePropertyNominalSampleRate;
            err = AudioObjectRemovePropertyListener(pStreamIn->deviceID, &propAdr,
                                                    coreAudioRecordingAudioDevicePropertyChanged, &pStreamIn->Common);
            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to remove the recording sample rate changed listener (%RI32)\n", err));
            }

            if (pStreamIn->fDefDevChgListReg)
            {
                propAdr.mSelector = kAudioHardwarePropertyDefaultInputDevice;
                err = AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &propAdr,
                                                        coreAudioDefaultDeviceChanged, &pStreamIn->Common);
                if (   err != noErr
                    && err != kAudioHardwareBadObjectError)
                {
                    LogRel(("CoreAudio: Failed to remove the default recording device changed listener (%RI32)\n", err));
                }

                pStreamIn->fDefDevChgListReg = false;
            }

            if (pStreamIn->fDevStateChgListReg)
            {
                Assert(pStreamIn->deviceID != kAudioDeviceUnknown);

                AudioObjectPropertyAddress propAdr2 = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                                        kAudioObjectPropertyElementMaster };
                err = AudioObjectRemovePropertyListener(pStreamIn->deviceID, &propAdr2,
                                                        drvHostCoreAudioDeviceStateChanged, &pStreamIn->Common);
                if (   err != noErr
                    && err != kAudioHardwareBadObjectError)
                {
                    LogRel(("CoreAudio: Failed to remove the recording device state changed listener (%RI32)\n", err));
                }

                pStreamIn->fDevStateChgListReg = false;
            }

#ifndef VBOX_WITH_AUDIO_CA_QUEUES
            if (pStreamIn->pConverter)
            {
                AudioConverterDispose(pStreamIn->pConverter);
                pStreamIn->pConverter = NULL;
            }

            err = AudioUnitUninitialize(pStreamIn->audioUnit);
            if (err == noErr)
                err = AudioComponentInstanceDispose(pStreamIn->audioUnit);

            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to uninit the recording device (%RI32)\n", err));
            }

            pStreamIn->deviceID      = kAudioDeviceUnknown;
            pStreamIn->audioUnit     = NULL;
            pStreamIn->sampleRatio   = 1;

# ifdef VBOX_WITH_AUDIO_CA_CONVERTER
            coreAudioUninitConvCbCtx(&pStreamIn->convCbCtx);
# endif
#else /* VBOX_WITH_AUDIO_CA_QUEUES */
            if (RTCritSectIsInitialized(&pCAStream->CritSect))
                RTCritSectDelete(&pCAStream->CritSect);
#endif
            if (pStreamIn->pCircBuf)
            {
                RTCircBufDestroy(pStreamIn->pCircBuf);
                pStreamIn->pCircBuf = NULL;
            }

            ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_UNINIT);
        }
    }
    else
    {
        LogRel(("CoreAudio: Failed to stop recording on uninit (%RI32)\n", err));
        rc = VERR_GENERAL_FAILURE; /** @todo Fudge! */
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioDestroyStreamOut(PPDMAUDIOSTREAM pStream)
{
    PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;
    PCOREAUDIOSTREAM    pCAStream  = &pStreamOut->Common;

    LogFlowFuncEnter();

    uint32_t status = ASMAtomicReadU32(&pStreamOut->status);
    if (!(   status == CA_STATUS_INIT
          || status == CA_STATUS_REINIT))
    {
        return VINF_SUCCESS;
    }

    int rc = coreAudioControlStreamOut(pStreamOut, PDMAUDIOSTREAMCMD_DISABLE);
    if (RT_SUCCESS(rc))
    {
        ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_IN_UNINIT);

        rc = coreAudioStreamUninit(pCAStream);
        if (RT_SUCCESS(rc))
        {
            OSStatus err;

            /*
             * Unregister playback device callbacks.
             */
            AudioObjectPropertyAddress propAdr = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMaster };
#ifdef DEBUG
            err = AudioObjectRemovePropertyListener(pStreamOut->deviceID, &propAdr,
                                                    coreAudioPlaybackAudioDevicePropertyChanged, &pStreamOut->Common);
            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to remove the playback processor overload listener (%RI32)\n", err));
            }
#endif /* DEBUG */

            propAdr.mSelector = kAudioDevicePropertyNominalSampleRate;
            err = AudioObjectRemovePropertyListener(pStreamOut->deviceID, &propAdr,
                                                    coreAudioPlaybackAudioDevicePropertyChanged, &pStreamOut->Common);
            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to remove the playback sample rate changed listener (%RI32)\n", err));
            }

            if (pStreamOut->fDefDevChgListReg)
            {
                propAdr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
                propAdr.mScope    = kAudioObjectPropertyScopeGlobal;
                propAdr.mElement  = kAudioObjectPropertyElementMaster;
                err = AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &propAdr,
                                                        coreAudioDefaultDeviceChanged, &pStreamOut->Common);
                if (   err != noErr
                    && err != kAudioHardwareBadObjectError)
                {
                    LogRel(("CoreAudio: Failed to remove the default playback device changed listener (%RI32)\n", err));
                }

                pStreamOut->fDefDevChgListReg = false;
            }

            if (pStreamOut->fDevStateChgListReg)
            {
                Assert(pStreamOut->deviceID != kAudioDeviceUnknown);

                AudioObjectPropertyAddress propAdr2 = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                                        kAudioObjectPropertyElementMaster };
                err = AudioObjectRemovePropertyListener(pStreamOut->deviceID, &propAdr2,
                                                        drvHostCoreAudioDeviceStateChanged, &pStreamOut->Common);
                if (   err != noErr
                    && err != kAudioHardwareBadObjectError)
                {
                    LogRel(("CoreAudio: Failed to remove the playback device state changed listener (%RI32)\n", err));
                }

                pStreamOut->fDevStateChgListReg = false;
            }

#ifndef VBOX_WITH_AUDIO_CA_QUEUES
            err = AudioUnitUninitialize(pStreamOut->audioUnit);
            if (err == noErr)
                err = AudioComponentInstanceDispose(pStreamOut->audioUnit);

            if (   err != noErr
                && err != kAudioHardwareBadObjectError)
            {
                LogRel(("CoreAudio: Failed to uninit the playback device (%RI32)\n", err));
            }

            pStreamOut->deviceID  = kAudioDeviceUnknown;
            pStreamOut->audioUnit = NULL;
#else
            if (RTCritSectIsInitialized(&pCAStream->CritSect))
                RTCritSectDelete(&pCAStream->CritSect);
#endif
            if (pStreamOut->pCircBuf)
            {
                RTCircBufDestroy(pStreamOut->pCircBuf);
                pStreamOut->pCircBuf = NULL;
            }

            ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_UNINIT);
        }
    }
    else
        LogRel(("CoreAudio: Failed to stop playback on uninit, rc=%Rrc\n", rc));

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioCreateStreamIn(PDRVHOSTCOREAUDIO pThis,
                                   PPDMAUDIOSTREAM pStream, PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    AssertPtrReturn(pThis,   VERR_INVALID_POINTER);
    AssertPtrReturn(pStream, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgReq, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgAcq, VERR_INVALID_POINTER);

    PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;

    LogFlowFunc(("enmRecSource=%RU32\n", pCfgReq->DestSource.Source));

    pStreamIn->deviceID            = kAudioDeviceUnknown;
    pStreamIn->audioUnit           = NULL;
    pStreamIn->pConverter          = NULL;
    pStreamIn->sampleRatio         = 1;
    pStreamIn->pCircBuf            = NULL;
    pStreamIn->status              = CA_STATUS_UNINIT;
    pStreamIn->fDefDevChgListReg   = false;
    pStreamIn->fDevStateChgListReg = false;

    PCOREAUDIOSTREAM pCAStream = &pStreamIn->Common;

    int rc;

    pCAStream->pThis  = pThis;
    pCAStream->enmDir = PDMAUDIODIR_IN;
    pCAStream->pIn    = pStreamIn;
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    rc = RTCritSectInit(&pCAStream->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    pCAStream->hThread    = NIL_RTTHREAD;
    pCAStream->fRun       = false;
    pCAStream->fIsRunning = false;
    pCAStream->fShutdown  = false;
#endif

    bool fDeviceByUser = false; /* Do we use a device which was set by the user? */

#if 0
    /* Try to find the audio device set by the user */
    if (DeviceUID.pszInputDeviceUID)
    {
        pStreamIn->deviceID = drvHostCoreAudioDeviceUIDtoID(DeviceUID.pszInputDeviceUID);
        /* Not fatal */
        if (pStreamIn->deviceID == kAudioDeviceUnknown)
            LogRel(("CoreAudio: Unable to find recording device %s. Falling back to the default audio device. \n", DeviceUID.pszInputDeviceUID));
        else
            fDeviceByUser = true;
    }
#endif

    ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_IN_INIT);

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    rc = coreAudioStreamInitQueue(pCAStream, true /* fIn */, pCfgReq, pCfgAcq);
    if (RT_SUCCESS(rc))
    {
        pCfgAcq->cSampleBufferSize = _4K; /** @todo FIX THIS !!! */
    }
#else
    rc = coreAudioStreamInitIn(pStreamIn, pCfgReq, pCfgAcq);
#endif
    if (RT_SUCCESS(rc))
    {
        ASMAtomicXchgU32(&pStreamIn->status, CA_STATUS_INIT);

        OSStatus err;

        /* When the devices isn't forced by the user, we want default device change notifications. */
        if (!fDeviceByUser)
        {
            if (!pStreamIn->fDefDevChgListReg)
            {
                AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
                                                       kAudioObjectPropertyElementMaster };
                err = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &propAdr,
                                                     coreAudioDefaultDeviceChanged, &pStreamIn->Common);
                if (   err == noErr
                    || err == kAudioHardwareIllegalOperationError)
                {
                    pStreamIn->fDefDevChgListReg = true;
                }
                else
                    LogRel(("CoreAudio: Failed to add the default recording device changed listener (%RI32)\n", err));
            }
        }

        if (   !pStreamIn->fDevStateChgListReg
            && (pStreamIn->deviceID != kAudioDeviceUnknown))
        {
            /* Register callback for being notified if the device stops being alive. */
            AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMaster };
            err = AudioObjectAddPropertyListener(pStreamIn->deviceID, &propAdr, drvHostCoreAudioDeviceStateChanged,
                                                 &pStreamIn->Common);
            if (err == noErr)
            {
                pStreamIn->fDevStateChgListReg = true;
            }
            else
                LogRel(("CoreAudio: Failed to add the recording device state changed listener (%RI32)\n", err));
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int coreAudioCreateStreamOut(PDRVHOSTCOREAUDIO pThis,
                                    PPDMAUDIOSTREAM pStream, PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    AssertPtrReturn(pThis,   VERR_INVALID_POINTER);
    AssertPtrReturn(pStream, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgReq, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgAcq, VERR_INVALID_POINTER);

    PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;

    LogFlowFuncEnter();

    pStreamOut->deviceID            = kAudioDeviceUnknown;
    pStreamOut->audioUnit           = NULL;
    pStreamOut->pCircBuf            = NULL;
    pStreamOut->status              = CA_STATUS_UNINIT;
    pStreamOut->fDefDevChgListReg   = false;
    pStreamOut->fDevStateChgListReg = false;

    PCOREAUDIOSTREAM pCAStream = &pStreamOut->Common;

    int rc;

    pCAStream->pThis  = pThis;
    pCAStream->enmDir = PDMAUDIODIR_OUT;
    pCAStream->pOut   = pStreamOut;
#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    rc = RTCritSectInit(&pCAStream->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    pCAStream->hThread    = NIL_RTTHREAD;
    pCAStream->fRun       = false;
    pCAStream->fIsRunning = false;
    pCAStream->fShutdown  = false;
#endif

    bool fDeviceByUser = false; /* Do we use a device which was set by the user? */

#if 0
    /* Try to find the audio device set by the user. Use
     * export VBOX_COREAUDIO_OUTPUT_DEVICE_UID=AppleHDAEngineOutput:0
     * to set it. */
    if (DeviceUID.pszOutputDeviceUID)
    {
        pStreamOut->audioDeviceId = drvHostCoreAudioDeviceUIDtoID(DeviceUID.pszOutputDeviceUID);
        /* Not fatal */
        if (pStreamOut->audioDeviceId == kAudioDeviceUnknown)
            LogRel(("CoreAudio: Unable to find playback device %s. Falling back to the default audio device. \n", DeviceUID.pszOutputDeviceUID));
        else
            fDeviceByUser = true;
    }
#endif

#ifdef VBOX_WITH_AUDIO_CA_QUEUES
    rc = coreAudioStreamInitQueue(pCAStream, false /* fIn */, pCfgReq, pCfgAcq);
    if (RT_SUCCESS(rc))
    {
        pCfgAcq->cSampleBufferSize = _4K; /** @todo FIX THIS !!! */
    }
#else
    rc = coreAudioStreamInitOut(pStreamOut, pCfgReq, pCfgAcq);
#endif
    if (RT_SUCCESS(rc))
    {
        ASMAtomicXchgU32(&pStreamOut->status, CA_STATUS_INIT);

        OSStatus err;

        /* When the devices isn't forced by the user, we want default device change notifications. */
        if (!fDeviceByUser)
        {
            AudioObjectPropertyAddress propAdr = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMaster };
            err = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &propAdr,
                                                 coreAudioDefaultDeviceChanged, &pStreamOut->Common);
            if (err == noErr)
            {
                pStreamOut->fDefDevChgListReg = true;
            }
            else
                LogRel(("CoreAudio: Failed to add the default playback device changed listener (%RI32)\n", err));
        }

        if (   !pStreamOut->fDevStateChgListReg
            && (pStreamOut->deviceID != kAudioDeviceUnknown))
        {
            /* Register callback for being notified if the device stops being alive. */
            AudioObjectPropertyAddress propAdr = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMaster };
            err = AudioObjectAddPropertyListener(pStreamOut->deviceID, &propAdr, drvHostCoreAudioDeviceStateChanged,
                                                 (void *)&pStreamOut->Common);
            if (err == noErr)
            {
                pStreamOut->fDevStateChgListReg = true;
            }
            else
                LogRel(("CoreAudio: Failed to add the playback device state changed listener (%RI32)\n", err));
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnGetConfig}
 */
static DECLCALLBACK(int) drvHostCoreAudioGetConfig(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDCFG pBackendCfg)
{
    AssertPtrReturn(pInterface,  VERR_INVALID_POINTER);
    AssertPtrReturn(pBackendCfg, VERR_INVALID_POINTER);

    PDRVHOSTCOREAUDIO pThis = PDMIHOSTAUDIO_2_DRVHOSTCOREAUDIO(pInterface);

    return coreAudioUpdateStatusInternalEx(pThis, pBackendCfg, 0 /* fEnum */);
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnStreamGetStatus}
 */
static DECLCALLBACK(PDMAUDIOBACKENDSTS) drvHostCoreAudioGetStatus(PPDMIHOSTAUDIO pInterface, PDMAUDIODIR enmDir)
{
    RT_NOREF(enmDir);
    AssertPtrReturn(pInterface, PDMAUDIOBACKENDSTS_UNKNOWN);

    return PDMAUDIOBACKENDSTS_RUNNING;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnStreamCreate}
 */
static DECLCALLBACK(int) drvHostCoreAudioStreamCreate(PPDMIHOSTAUDIO pInterface,
                                                      PPDMAUDIOSTREAM pStream,
                                                      PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgReq,    VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgAcq,    VERR_INVALID_POINTER);

    PDRVHOSTCOREAUDIO pThis = PDMIHOSTAUDIO_2_DRVHOSTCOREAUDIO(pInterface);

    int rc;
    if (pCfgReq->enmDir == PDMAUDIODIR_IN)
        rc = coreAudioCreateStreamIn(pThis,  pStream, pCfgReq, pCfgAcq);
    else
        rc = coreAudioCreateStreamOut(pThis, pStream, pCfgReq, pCfgAcq);

    LogFlowFunc(("%s: rc=%Rrc\n", pStream->szName, rc));
    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnStreamDestroy}
 */
static DECLCALLBACK(int) drvHostCoreAudioStreamDestroy(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);

    int rc;
    if (pStream->enmDir == PDMAUDIODIR_IN)
        rc = coreAudioDestroyStreamIn(pStream);
    else
        rc = coreAudioDestroyStreamOut(pStream);

    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnStreamControl}
 */
static DECLCALLBACK(int) drvHostCoreAudioStreamControl(PPDMIHOSTAUDIO pInterface,
                                                       PPDMAUDIOSTREAM pStream, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);

    Assert(pStream->enmCtx == PDMAUDIOSTREAMCTX_HOST);

    int rc;
    if (pStream->enmDir == PDMAUDIODIR_IN)
        rc = coreAudioControlStreamIn((PCOREAUDIOSTREAMIN)pStream, enmStreamCmd);
    else
        rc = coreAudioControlStreamOut((PCOREAUDIOSTREAMOUT)pStream, enmStreamCmd);

    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnStreamGetStatus}
 */
static DECLCALLBACK(PDMAUDIOSTRMSTS) drvHostCoreAudioStreamGetStatus(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream)
{
    RT_NOREF(pInterface);
    AssertPtrReturn(pStream, VERR_INVALID_POINTER);

    Assert(pStream->enmCtx == PDMAUDIOSTREAMCTX_HOST);

    PDMAUDIOSTRMSTS strmSts = PDMAUDIOSTRMSTS_FLAG_NONE;

    if (pStream->enmDir == PDMAUDIODIR_IN)
    {
        PCOREAUDIOSTREAMIN pStreamIn = (PCOREAUDIOSTREAMIN)pStream;

        if (ASMAtomicReadU32(&pStreamIn->status) == CA_STATUS_INIT)
            strmSts |= PDMAUDIOSTRMSTS_FLAG_INITIALIZED | PDMAUDIOSTRMSTS_FLAG_ENABLED;

        if (strmSts & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            strmSts |= PDMAUDIOSTRMSTS_FLAG_DATA_READABLE;
    }
    else if (pStream->enmDir == PDMAUDIODIR_OUT)
    {
        PCOREAUDIOSTREAMOUT pStreamOut = (PCOREAUDIOSTREAMOUT)pStream;

        if (ASMAtomicReadU32(&pStreamOut->status) == CA_STATUS_INIT)
            strmSts |= PDMAUDIOSTRMSTS_FLAG_INITIALIZED | PDMAUDIOSTRMSTS_FLAG_ENABLED;

        if (strmSts & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            strmSts |= PDMAUDIOSTRMSTS_FLAG_DATA_WRITABLE;
    }
    else
        AssertFailed();

    return strmSts;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnStreamIterate}
 */
static DECLCALLBACK(int) drvHostCoreAudioStreamIterate(PPDMIHOSTAUDIO pInterface, PPDMAUDIOSTREAM pStream)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pStream,    VERR_INVALID_POINTER);

    /* Nothing to do here for Core Audio. */
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO, pfnShutdown}
 */
static DECLCALLBACK(void) drvHostCoreAudioShutdown(PPDMIHOSTAUDIO pInterface)
{
    RT_NOREF(pInterface);
}


/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) drvHostCoreAudioQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS          pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTCOREAUDIO   pThis   = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIHOSTAUDIO, &pThis->IHostAudio);

    return NULL;
}

/**
 * @callback_method_impl{FNPDMDRVCONSTRUCT,
 *      Construct a DirectSound Audio driver instance.}
 */
static DECLCALLBACK(int) drvHostCoreAudioConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    RT_NOREF(pCfg, fFlags);
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVHOSTCOREAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTCOREAUDIO);
    LogRel(("Audio: Initializing Core Audio driver\n"));

    /*
     * Init the static parts.
     */
    pThis->pDrvIns                   = pDrvIns;
    /* IBase */
    pDrvIns->IBase.pfnQueryInterface = drvHostCoreAudioQueryInterface;
    /* IHostAudio */
    PDMAUDIO_IHOSTAUDIO_CALLBACKS(drvHostCoreAudio);

    return VINF_SUCCESS;
}

/**
 * Char driver registration record.
 */
const PDMDRVREG g_DrvHostCoreAudio =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "CoreAudio",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Core Audio host driver",
    /* fFlags */
     PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_AUDIO,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVHOSTCOREAUDIO),
    /* pfnConstruct */
    drvHostCoreAudioConstruct,
    /* pfnDestruct */
    NULL,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};
