/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AudioStreamBuilder"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <new>
#include <stdint.h>
#include <vector>

#include <aaudio/AAudio.h>
#include <aaudio/AAudioTesting.h>
#include <android/media/audio/common/AudioMMapPolicy.h>
#include <android/media/audio/common/AudioMMapPolicyInfo.h>
#include <android/media/audio/common/AudioMMapPolicyType.h>
#include <media/AudioSystem.h>

#include "binding/AAudioBinderClient.h"
#include "client/AudioStreamInternalCapture.h"
#include "client/AudioStreamInternalPlay.h"
#include "core/AudioGlobal.h"
#include "core/AudioStream.h"
#include "core/AudioStreamBuilder.h"
#include "legacy/AudioStreamRecord.h"
#include "legacy/AudioStreamTrack.h"

using namespace aaudio;

using android::media::audio::common::AudioMMapPolicy;
using android::media::audio::common::AudioMMapPolicyInfo;
using android::media::audio::common::AudioMMapPolicyType;

#define AAUDIO_MMAP_POLICY_DEFAULT             AAUDIO_POLICY_NEVER
#define AAUDIO_MMAP_EXCLUSIVE_POLICY_DEFAULT   AAUDIO_POLICY_NEVER

// These values are for a pre-check before we ask the lower level service to open a stream.
// So they are just outside the maximum conceivable range of value,
// on the edge of being ridiculous.
// TODO These defines should be moved to a central place in audio.
#define SAMPLES_PER_FRAME_MIN        1
#define SAMPLES_PER_FRAME_MAX        FCC_LIMIT
#define SAMPLE_RATE_HZ_MIN           8000
// HDMI supports up to 32 channels at 1536000 Hz.
#define SAMPLE_RATE_HZ_MAX           1600000
#define FRAMES_PER_DATA_CALLBACK_MIN 1
#define FRAMES_PER_DATA_CALLBACK_MAX (1024 * 1024)

/*
 * AudioStreamBuilder
 */
static aaudio_result_t builder_createStream(aaudio_direction_t direction,
                                            aaudio_sharing_mode_t /*sharingMode*/,
                                            bool tryMMap,
                                            android::sp<AudioStream> &stream) {
    aaudio_result_t result = AAUDIO_OK;

    switch (direction) {

        case AAUDIO_DIRECTION_INPUT:
            if (tryMMap) {
                stream = new AudioStreamInternalCapture(AAudioBinderClient::getInstance(),
                                                                 false);
            } else {
                stream = new AudioStreamRecord();
            }
            break;

        case AAUDIO_DIRECTION_OUTPUT:
            if (tryMMap) {
                stream = new AudioStreamInternalPlay(AAudioBinderClient::getInstance(),
                                                              false);
            } else {
                stream = new AudioStreamTrack();
            }
            break;

        default:
            ALOGE("%s() bad direction = %d", __func__, direction);
            result = AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
    return result;
}

namespace {

aaudio_policy_t aidl2legacy_aaudio_policy(AudioMMapPolicy aidl) {
    switch (aidl) {
        case AudioMMapPolicy::NEVER:
            return AAUDIO_POLICY_NEVER;
        case AudioMMapPolicy::AUTO:
            return AAUDIO_POLICY_AUTO;
        case AudioMMapPolicy::ALWAYS:
            return AAUDIO_POLICY_ALWAYS;
        case AudioMMapPolicy::UNSPECIFIED:
        default:
            return AAUDIO_UNSPECIFIED;
    }
}

// The aaudio policy will be ALWAYS, NEVER, UNSPECIFIED only when all policy info are
// ALWAYS, NEVER or UNSPECIFIED. Otherwise, the aaudio policy will be AUTO.
aaudio_policy_t getAAudioPolicy(
        const std::vector<AudioMMapPolicyInfo>& policyInfos) {
    if (policyInfos.empty()) return AAUDIO_POLICY_AUTO;
    for (size_t i = 1; i < policyInfos.size(); ++i) {
        if (policyInfos.at(i).mmapPolicy != policyInfos.at(0).mmapPolicy) {
            return AAUDIO_POLICY_AUTO;
        }
    }
    return aidl2legacy_aaudio_policy(policyInfos.at(0).mmapPolicy);
}

} // namespace

// Try to open using MMAP path if that is allowed.
// Fall back to Legacy path if MMAP not available.
// Exact behavior is controlled by MMapPolicy.
aaudio_result_t AudioStreamBuilder::build(AudioStream** streamPtr) {

    if (streamPtr == nullptr) {
        ALOGE("%s() streamPtr is null", __func__);
        return AAUDIO_ERROR_NULL;
    }
    *streamPtr = nullptr;

    logParameters();

    aaudio_result_t result = validate();
    if (result != AAUDIO_OK) {
        return result;
    }

    std::vector<AudioMMapPolicyInfo> policyInfos;
    // The API setting is the highest priority.
    aaudio_policy_t mmapPolicy = AudioGlobal_getMMapPolicy();
    // If not specified then get from a system property.
    if (mmapPolicy == AAUDIO_UNSPECIFIED && android::AudioSystem::getMmapPolicyInfo(
                AudioMMapPolicyType::DEFAULT, &policyInfos) == NO_ERROR) {
        mmapPolicy = getAAudioPolicy(policyInfos);
    }
    // If still not specified then use the default.
    if (mmapPolicy == AAUDIO_UNSPECIFIED) {
        mmapPolicy = AAUDIO_MMAP_POLICY_DEFAULT;
    }

    policyInfos.clear();
    aaudio_policy_t mmapExclusivePolicy = AAUDIO_UNSPECIFIED;
    if (android::AudioSystem::getMmapPolicyInfo(
            AudioMMapPolicyType::EXCLUSIVE, &policyInfos) == NO_ERROR) {
        mmapExclusivePolicy = getAAudioPolicy(policyInfos);
    }
    if (mmapExclusivePolicy == AAUDIO_UNSPECIFIED) {
        mmapExclusivePolicy = AAUDIO_MMAP_EXCLUSIVE_POLICY_DEFAULT;
    }

    aaudio_sharing_mode_t sharingMode = getSharingMode();
    if ((sharingMode == AAUDIO_SHARING_MODE_EXCLUSIVE)
        && (mmapExclusivePolicy == AAUDIO_POLICY_NEVER)) {
        ALOGD("%s() EXCLUSIVE sharing mode not supported. Use SHARED.", __func__);
        sharingMode = AAUDIO_SHARING_MODE_SHARED;
        setSharingMode(sharingMode);
    }

    bool allowMMap = mmapPolicy != AAUDIO_POLICY_NEVER;
    bool allowLegacy = mmapPolicy != AAUDIO_POLICY_ALWAYS;

    // TODO Support other performance settings in MMAP mode.
    // Disable MMAP if low latency not requested.
    if (getPerformanceMode() != AAUDIO_PERFORMANCE_MODE_LOW_LATENCY) {
        ALOGD("%s() MMAP not used because AAUDIO_PERFORMANCE_MODE_LOW_LATENCY not requested.",
              __func__);
        allowMMap = false;
    }

    // SessionID and Effects are only supported in Legacy mode.
    if (getSessionId() != AAUDIO_SESSION_ID_NONE) {
        ALOGD("%s() MMAP not used because sessionId specified.", __func__);
        allowMMap = false;
    }

    if (!allowMMap && !allowLegacy) {
        ALOGE("%s() no backend available: neither MMAP nor legacy path are allowed", __func__);
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }

    setPrivacySensitive(false);
    if (mPrivacySensitiveReq == PRIVACY_SENSITIVE_DEFAULT) {
        // When not explicitly requested, set privacy sensitive mode according to input preset:
        // communication and camcorder captures are considered privacy sensitive by default.
        aaudio_input_preset_t preset = getInputPreset();
        if (preset == AAUDIO_INPUT_PRESET_CAMCORDER
                || preset == AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION) {
            setPrivacySensitive(true);
        }
    } else if (mPrivacySensitiveReq == PRIVACY_SENSITIVE_ENABLED) {
        setPrivacySensitive(true);
    }

    android::sp<AudioStream> audioStream;
    result = builder_createStream(getDirection(), sharingMode, allowMMap, audioStream);
    if (result == AAUDIO_OK) {
        // Open the stream using the parameters from the builder.
        result = audioStream->open(*this);
        if (result != AAUDIO_OK) {
            bool isMMap = audioStream->isMMap();
            if (isMMap && allowLegacy) {
                ALOGV("%s() MMAP stream did not open so try Legacy path", __func__);
                // If MMAP stream failed to open then TRY using a legacy stream.
                result = builder_createStream(getDirection(), sharingMode,
                                              false, audioStream);
                if (result == AAUDIO_OK) {
                    result = audioStream->open(*this);
                }
            }
        }
        if (result == AAUDIO_OK) {
            audioStream->registerPlayerBase();
            audioStream->logOpenActual();
            *streamPtr = startUsingStream(audioStream);
        } // else audioStream will go out of scope and be deleted
    }

    return result;
}

AudioStream *AudioStreamBuilder::startUsingStream(android::sp<AudioStream> &audioStream) {
    // Increment the smart pointer so it will not get deleted when
    // we pass it to the C caller and it goes out of scope.
    // The C code cannot hold a smart pointer so we increment the reference
    // count to indicate that the C app owns a reference.
    audioStream->incStrong(nullptr);
    return audioStream.get();
}

void AudioStreamBuilder::stopUsingStream(AudioStream *stream) {
    // Undo the effect of startUsingStream()
    android::sp<AudioStream> spAudioStream(stream);
    ALOGV("%s() strongCount = %d", __func__, spAudioStream->getStrongCount());
    spAudioStream->decStrong(nullptr);
}

aaudio_result_t AudioStreamBuilder::validate() const {

    // Check for values that are ridiculously out of range to prevent math overflow exploits.
    // The service will do a better check.
    aaudio_result_t result = AAudioStreamParameters::validate();
    if (result != AAUDIO_OK) {
        return result;
    }

    switch (mPerformanceMode) {
        case AAUDIO_PERFORMANCE_MODE_NONE:
        case AAUDIO_PERFORMANCE_MODE_POWER_SAVING:
        case AAUDIO_PERFORMANCE_MODE_LOW_LATENCY:
            break;
        default:
            ALOGE("illegal performanceMode = %d", mPerformanceMode);
            return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
            // break;
    }

    // Prevent ridiculous values from causing problems.
    if (mFramesPerDataCallback != AAUDIO_UNSPECIFIED
        && (mFramesPerDataCallback < FRAMES_PER_DATA_CALLBACK_MIN
            || mFramesPerDataCallback > FRAMES_PER_DATA_CALLBACK_MAX)) {
        ALOGE("framesPerDataCallback out of range = %d",
              mFramesPerDataCallback);
        return AAUDIO_ERROR_OUT_OF_RANGE;
    }

    return AAUDIO_OK;
}

static const char *AAudio_convertSharingModeToShortText(aaudio_sharing_mode_t sharingMode) {
    switch (sharingMode) {
        case AAUDIO_SHARING_MODE_EXCLUSIVE:
            return "EX";
        case AAUDIO_SHARING_MODE_SHARED:
            return "SH";
        default:
            return "?!";
    }
}

static const char *AAudio_convertDirectionToText(aaudio_direction_t direction) {
    switch (direction) {
        case AAUDIO_DIRECTION_OUTPUT:
            return "OUTPUT";
        case AAUDIO_DIRECTION_INPUT:
            return "INPUT";
        default:
            return "?!";
    }
}

void AudioStreamBuilder::logParameters() const {
    // This is very helpful for debugging in the future. Please leave it in.
    ALOGI("rate   = %6d, channels  = %d, channelMask = %#x, format   = %d, sharing = %s, dir = %s",
          getSampleRate(), getSamplesPerFrame(), getChannelMask(), getFormat(),
          AAudio_convertSharingModeToShortText(getSharingMode()),
          AAudio_convertDirectionToText(getDirection()));
    ALOGI("device = %6d, sessionId = %d, perfMode = %d, callback: %s with frames = %d",
          getDeviceId(),
          getSessionId(),
          getPerformanceMode(),
          ((getDataCallbackProc() != nullptr) ? "ON" : "OFF"),
          mFramesPerDataCallback);
    ALOGI("usage  = %6d, contentType = %d, inputPreset = %d, allowedCapturePolicy = %d",
          getUsage(), getContentType(), getInputPreset(), getAllowedCapturePolicy());
    ALOGI("privacy sensitive = %s", isPrivacySensitive() ? "true" : "false");
    ALOGI("opPackageName = %s", !getOpPackageName().has_value() ?
        "(null)" : getOpPackageName().value().c_str());
    ALOGI("attributionTag = %s", !getAttributionTag().has_value() ?
        "(null)" : getAttributionTag().value().c_str());
}
