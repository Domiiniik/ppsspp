// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// SAS is a software mixing engine that runs on the Media Engine CPU. We just HLE it.
// This is a very rough implementation that needs lots of work.
//
// This file just contains the API, the real stuff is in HW/SasAudio.cpp/h.
//
// JPCSP is, as it often is, a pretty good reference although I didn't actually use it much yet:
// http://code.google.com/p/jpcsp/source/browse/trunk/src/jpcsp/HLE/modules150/sceSasCore.java
//
// This should be multithreaded and improved at some point. Some discussion here:
// https://github.com/hrydgard/ppsspp/issues/1078

#include <cstdlib>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "Common/Profiler/Profiler.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"
#include "Core/MIPS/MIPS.h"
#include "Core/HW/SasAudio.h"
#include "Core/MemMap.h"
#include "Core/Reporting.h"

#include "Core/HLE/HLE.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceSas.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelThread.h"

// TODO - allow more than one, associating each with one Core pointer (passed in to all the functions)
// No known games use more than one instance of Sas though.
static SasInstance *sas;

SasInstance *GetSasInstance() {
	return sas;
}

enum SasThreadState {
	DISABLED,
	READY,
	QUEUED,
};
struct SasThreadParams {
	u32 outAddr;
	u32 inAddr;
	int leftVol;
	int rightVol;
};

static std::thread g_sasThread;
static std::mutex sasWakeMutex;
static std::mutex sasDoneMutex;
static std::condition_variable sasWake;
static std::condition_variable sasDone;
static volatile int sasThreadState = SasThreadState::DISABLED;
static SasThreadParams sasThreadParams;
static int sasMixEvent = -1;

static bool g_sasMuteFlag = false;

bool *__SasGetGlobalMuteFlag() {
	return &g_sasMuteFlag;
}

int __SasThread() {
	SetCurrentThreadName("SAS");

	std::unique_lock<std::mutex> guard(sasWakeMutex);
	while (sasThreadState != SasThreadState::DISABLED) {
		sasWake.wait(guard);
		if (sasThreadState == SasThreadState::QUEUED) {
			const bool mute = g_sasMuteFlag;
			sas->Mix(sasThreadParams.outAddr, sasThreadParams.inAddr, sasThreadParams.leftVol, sasThreadParams.rightVol, mute);
			std::lock_guard<std::mutex> doneGuard(sasDoneMutex);
			sasThreadState = SasThreadState::READY;
			sasDone.notify_one();
		}
	}
	return 0;
}

static void __SasDrain() {
	std::unique_lock<std::mutex> guard(sasDoneMutex);
	while (sasThreadState == SasThreadState::QUEUED)
		sasDone.wait(guard);
}

static void __SasEnqueueMix(u32 outAddr, u32 inAddr = 0, int leftVol = 0, int rightVol = 0) {
	if (sasThreadState == SasThreadState::DISABLED) {
		// No thread, call it immediately.
		const bool mute = g_sasMuteFlag;
		sas->Mix(outAddr, inAddr, leftVol, rightVol, mute);
		return;
	}

	if (sasThreadState == SasThreadState::QUEUED) {
		// Wait for the queue to drain.
		__SasDrain();
	}

	// We're safe to write, since it can't be processing now anymore.
	// No other thread enqueues.
	sasThreadParams.outAddr = outAddr;
	sasThreadParams.inAddr = inAddr;
	sasThreadParams.leftVol = leftVol;
	sasThreadParams.rightVol = rightVol;

	// And now, notify.
	sasWakeMutex.lock();
	sasThreadState = SasThreadState::QUEUED;
	sasWake.notify_one();
	sasWakeMutex.unlock();
}

static void __SasDisableThread() {
	if (sasThreadState != SasThreadState::DISABLED) {
		sasWakeMutex.lock();
		sasThreadState = SasThreadState::DISABLED;
		sasWake.notify_one();
		sasWakeMutex.unlock();
		if (g_sasThread.joinable()) {
			g_sasThread.join();
		}
	}
}

static void sasMixFinish(u64 userdata, int cycleslate) {
	PROFILE_THIS_SCOPE("mixer");

	u32 error;
	SceUID threadID = (SceUID)userdata;
	SceUID verify = __KernelGetWaitID(threadID, WAITTYPE_HLEDELAY, error);
	u64 result = __KernelGetWaitValue(threadID, error);

	if (error == 0 && verify == 1) {
		// Wait until it's actually complete before waking the thread.
		__SasDrain();

		__KernelResumeThreadFromWait(threadID, result);
		__KernelReSchedule("woke from sas mix");
	} else {
		WARN_LOG(Log::HLE, "Someone else woke up SAS-blocked thread?");
	}
}

void __SasInit() {
	sas = new SasInstance();

	g_sasMuteFlag = false;

	sasMixEvent = CoreTiming::RegisterEvent("SasMix", sasMixFinish);

	if (g_Config.bSeparateSASThread) {
		sasThreadState = SasThreadState::READY;
		g_sasThread = std::thread(__SasThread);
	} else {
		sasThreadState = SasThreadState::DISABLED;
	}
}

void __SasDoState(PointerWrap &p) {
	auto s = p.Section("sceSas", 1, 2);
	if (!s)
		return;

	if (sasThreadState == SasThreadState::QUEUED) {
		// Wait for the queue to drain.  Don't want to save the wrong stuff.
		__SasDrain();
	}

	DoClass(p, sas);

	if (s >= 2) {
		Do(p, sasMixEvent);
	} else {
		sasMixEvent = -1;
		__SasDisableThread();
	}

	CoreTiming::RestoreRegisterEvent(sasMixEvent, "SasMix", sasMixFinish);
}

void __SasShutdown() {
	__SasDisableThread();

	delete sas;
	sas = 0;
}

static u32 sceSasInit(u32 core, u32 grainSize, u32 maxVoices, u32 outputMode, u32 sampleRate) {
	if (!Memory::IsValidAddress(core) || (core & 0x3F) != 0) {
		ERROR_LOG_REPORT(Log::sceSas, "sceSasInit(%08x, %i, %i, %i, %i): bad core address", core, grainSize, maxVoices, outputMode, sampleRate);
		return hleNoLog(SCE_SAS_ERROR_BAD_ADDRESS);
	}
	if (maxVoices == 0 || maxVoices > PSP_SAS_VOICES_MAX) {
		ERROR_LOG_REPORT(Log::sceSas, "sceSasInit(%08x, %i, %i, %i, %i): bad max voices", core, grainSize, maxVoices, outputMode, sampleRate);
		return hleNoLog(SCE_SAS_ERROR_INVALID_MAX_VOICES);
	}
	if (grainSize < 0x40 || grainSize > 0x800 || (grainSize & 0x1F) != 0) {
		ERROR_LOG_REPORT(Log::sceSas, "sceSasInit(%08x, %i, %i, %i, %i): bad grain size", core, grainSize, maxVoices, outputMode, sampleRate);
		return hleNoLog(SCE_SAS_ERROR_INVALID_GRAIN);
	}
	if (outputMode != 0 && outputMode != 1) {
		ERROR_LOG_REPORT(Log::sceSas, "sceSasInit(%08x, %i, %i, %i, %i): bad output mode", core, grainSize, maxVoices, outputMode, sampleRate);
		return hleNoLog(SCE_SAS_ERROR_INVALID_OUTPUT_MODE);
	}
	if (sampleRate != 44100) {
		ERROR_LOG_REPORT(Log::sceSas, "sceSasInit(%08x, %i, %i, %i, %i): bad sample rate", core, grainSize, maxVoices, outputMode, sampleRate);
		return hleNoLog(SCE_SAS_ERROR_INVALID_SAMPLE_RATE);
	}

	sas->SetGrainSize(grainSize);
	// Seems like the maxVoices param is actually ignored for all intents and purposes.
	sas->maxVoices = PSP_SAS_VOICES_MAX;
	sas->outputMode = outputMode;
	for (int i = 0; i < sas->maxVoices; i++) {
		sas->voices[i].sampleRate = sampleRate;
		sas->voices[i].playing = false;
		sas->voices[i].loop = false;
	}
	return hleLogInfo(Log::sceSas, 0);
}

static u32 sceSasGetEndFlag(u32 core) {
	u32 endFlag = 0;
	__SasDrain();
	for (int i = 0; i < sas->maxVoices; i++) {
		if (!sas->voices[i].playing)
			endFlag |= (1 << i);
	}

	return hleLogVerbose(Log::sceSas, endFlag);
}

static int delaySasResult(int result) {
	const int usec = sas->EstimateMixUs();

	// No event, fall back.
	if (sasMixEvent == -1) {
		return hleDelayResult(result, "sas core", usec);
	}

	CoreTiming::ScheduleEvent(usToCycles(usec), sasMixEvent, __KernelGetCurThread());
	__KernelWaitCurThread(WAITTYPE_HLEDELAY, 1, result, 0, false, "sas core");
	return result;
}

// Runs the mixer
static u32 _sceSasCore(u32 core, u32 outAddr) {
	PROFILE_THIS_SCOPE("mixer");

	if (!Memory::IsValidAddress(outAddr)) {
		return hleReportError(Log::sceSas, SCE_SAS_ERROR_INVALID_PARAMETER, "invalid address");
	}
	if (!__KernelIsDispatchEnabled()) {
		return hleLogError(Log::sceSas, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	}

	__SasEnqueueMix(outAddr);

	return hleLogVerbose(Log::sceSas, delaySasResult(0));
}

// Another way of running the mixer, the inoutAddr should be both input and output
static u32 _sceSasCoreWithMix(u32 core, u32 inoutAddr, int leftVolume, int rightVolume) {
	PROFILE_THIS_SCOPE("mixer");

	if (!Memory::IsValidAddress(inoutAddr)) {
		return hleReportError(Log::sceSas, SCE_SAS_ERROR_INVALID_PARAMETER, "invalid address");
	}
	if (sas->outputMode == PSP_SAS_OUTPUTMODE_RAW) {
		return hleReportError(Log::sceSas, SCE_KERNEL_ERROR_BAD_ARGUMENT, "unsupported outputMode");
	}
	if (!__KernelIsDispatchEnabled()) {
		return hleLogError(Log::sceSas, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	}

	__SasEnqueueMix(inoutAddr, inoutAddr, leftVolume, rightVolume);

	return delaySasResult(hleLogVerbose(Log::sceSas, 0));
}

static u32 sceSasSetVoice(u32 core, int voiceNum, u32 vagAddr, int size, int loop) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)	{
		return hleLogVerbose(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voicenum");
	}

	if (size == 0 || ((u32)size & 0xF) != 0) {
		if (size == 0) {
			return hleLogDebug(Log::sceSas, SCE_SAS_ERROR_INVALID_PARAMETER, "invalid size %d", size);
		} else {
			return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_PARAMETER, "invalid size %d", size);
		}
	}
	if (loop != 0 && loop != 1) {
		WARN_LOG_REPORT(Log::sceSas, "%s: invalid loop mode %d", __FUNCTION__, loop);
		return hleNoLog(SCE_SAS_ERROR_INVALID_LOOP_POS);
	}

	if (!Memory::IsValidAddress(vagAddr)) {
		return hleLogError(Log::sceSas, 0, "Ignoring invalid VAG audio address %08x", vagAddr);
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	if (v.type == VOICETYPE_ATRAC3) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_ATRAC3_ALREADY_SET, "voice is already ATRAC3");
	}

	if (size < 0) {
		// POSSIBLE HACK
		// SetVoice with negative sizes returns OK (0) unlike SetVoicePCM, but should not
		// play any audio, it seems. So let's bail and not do anything.
		// Needs more rigorous testing perhaps, but this fixes issue https://github.com/hrydgard/ppsspp/issues/5652
		// while being fairly low risk to other games.
		size = 0;
		DEBUG_LOG(Log::sceSas, "sceSasSetVoice(%08x, %i, %08x, %i, %i) : HACK: Negative size changed to 0", core, voiceNum, vagAddr, size, loop);
	} else {
		DEBUG_LOG(Log::sceSas, "sceSasSetVoice(%08x, %i, %08x, %i, %i)", core, voiceNum, vagAddr, size, loop);
	}

	u32 prevVagAddr = v.vagAddr;
	bool reset = false;
	if (v.type != VOICETYPE_VAG || v.vagAddr != vagAddr || v.vagSize != size || v.loop != (loop != 0)) {
		v.type = VOICETYPE_VAG;
		reset = true;
	}
	v.vagAddr = vagAddr;  // Real VAG header is 0x30 bytes behind the vagAddr
	v.vagSize = size;
	v.loop = loop != 0;
	if (v.on) {
		v.playing = true;
	}
	if (reset) {
		v.vag.Start(vagAddr, size, loop != 0);
	}
	return hleNoLog(0);
}

static u32 sceSasSetVoicePCM(u32 core, int voiceNum, u32 pcmAddr, int size, int loopPos) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voicenum");
	}
	if (size <= 0 || size > 0x10000) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_PCM_SIZE, "invalid size %d", size);
	}
	if (loopPos >= size) {
		ERROR_LOG_REPORT(Log::sceSas, "sceSasSetVoicePCM(%08x, %i, %08x, %i, %i): bad loop pos", core, voiceNum, pcmAddr, size, loopPos);
		return hleNoLog(SCE_SAS_ERROR_INVALID_LOOP_POS);
	}
	if (!Memory::IsValidAddress(pcmAddr)) {
		return hleLogError(Log::sceSas, 0, "Ignoring invalid PCM audio address %08x", pcmAddr);
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	if (v.type == VOICETYPE_ATRAC3) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_ATRAC3_ALREADY_SET, "voice is already ATRAC3");
	}

	u32 prevPcmAddr = v.pcmAddr;
	v.type = VOICETYPE_PCM;
	v.pcmAddr = pcmAddr;
	v.pcmSize = size;
	v.pcmIndex = 0;
	v.pcmLoopPos = loopPos >= 0 ? loopPos : 0;
	v.loop = loopPos >= 0 ? true : false;
	v.playing = true;
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasGetPauseFlag(u32 core) {
	u32 pauseFlag = 0;
	__SasDrain();
	for (int i = 0; i < sas->maxVoices; i++) {
		if (sas->voices[i].paused)
			pauseFlag |= (1 << i);
	}

	return hleLogDebug(Log::sceSas, pauseFlag);
}

static u32 sceSasSetPause(u32 core, u32 voicebit, int pause) {
	__SasDrain();
	for (int i = 0; voicebit != 0; i++, voicebit >>= 1) {
		if (i < PSP_SAS_VOICES_MAX && i >= 0) {
			if ((voicebit & 1) != 0)
				sas->voices[i].paused = pause != 0;
		}
	}

	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasSetVolume(u32 core, int voiceNum, int leftVol, int rightVol, int effectLeftVol, int effectRightVol) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)	{
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voicenum");
	}
	bool overVolume = abs(leftVol) > PSP_SAS_VOL_MAX || abs(rightVol) > PSP_SAS_VOL_MAX;
	overVolume = overVolume || abs(effectLeftVol) > PSP_SAS_VOL_MAX || abs(effectRightVol) > PSP_SAS_VOL_MAX;
	if (overVolume) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_INVALID_VOLUME);
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	v.volumeLeft = leftVol;
	v.volumeRight = rightVol;
	v.effectLeft = effectLeftVol;
	v.effectRight = effectRightVol;
	return hleLogVerbose(Log::sceSas, 0);
}

static u32 sceSasSetPitch(u32 core, int voiceNum, int pitch) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)	{
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	}
	if (pitch < PSP_SAS_PITCH_MIN || pitch > PSP_SAS_PITCH_MAX) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_PITCH, "bad pitch");
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	v.pitch = pitch;
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasSetKeyOn(u32 core, int voiceNum) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)	{
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	}

	__SasDrain();
	if (sas->voices[voiceNum].paused || sas->voices[voiceNum].on) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_VOICE_PAUSED);
	}

	SasVoice &v = sas->voices[voiceNum];
	v.KeyOn();
	return hleLogDebug(Log::sceSas, 0);
}

// sceSasSetKeyOff can be used to start sounds, that just sound during the Release phase!
static u32 sceSasSetKeyOff(u32 core, int voiceNum) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	} else {
		__SasDrain();
		if (sas->voices[voiceNum].paused || !sas->voices[voiceNum].on) {
			return hleLogDebug(Log::sceSas, SCE_SAS_ERROR_VOICE_PAUSED);  // this is ok
		}

		SasVoice &v = sas->voices[voiceNum];
		v.KeyOff();
		return hleLogDebug(Log::sceSas, 0);
	}
}

static u32 sceSasSetNoise(u32 core, int voiceNum, int freq) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	}
	if (freq < 0 || freq >= 64) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_NOISE_FREQ);
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	v.type = VOICETYPE_NOISE;
	v.noiseFreq = freq;
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasSetSL(u32 core, int voiceNum, int level) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0)	{
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	v.envelope.SetSustainLevel(level);
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasSetADSR(u32 core, int voiceNum, int flag, int a, int d, int s, int r) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	}
	// Create a mask like flag for the invalid values.
	int invalid = (a < 0 ? 0x1 : 0) | (d < 0 ? 0x2 : 0) | (s < 0 ? 0x4 : 0) | (r < 0 ? 0x8 : 0);
	if (invalid & flag) {
		WARN_LOG_REPORT(Log::sceSas, "sceSasSetADSR(%08x, %i, %i, %08x, %08x, %08x, %08x): invalid value", core, voiceNum, flag, a, d, s, r);
		return hleNoLog(SCE_SAS_ERROR_INVALID_ADSR_RATE);
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	v.envelope.SetRate(flag, a, d, s, r);
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasSetADSRMode(u32 core, int voiceNum, int flag, int a, int d, int s, int r) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	}

	// Probably by accident (?), the PSP ignores the top bit of these values.
	a = a & ~0x80000000;
	d = d & ~0x80000000;
	s = s & ~0x80000000;
	r = r & ~0x80000000;

	// This will look like the update flag for the invalid modes.
	int invalid = 0;
	if (a > 5 || (a & 1) != 0) {
		invalid |= 0x1;
	}
	if (d > 5 || (d & 1) != 1) {
		invalid |= 0x2;
	}
	if (s > 5) {
		invalid |= 0x4;
	}
	if (r > 5 || (r & 1) != 1) {
		invalid |= 0x8;
	}
	if (invalid & flag) {
		if (a == 5 && d == 5 && s == 5 && r == 5) {
			// Some games do this right at init.  It seems to fail even on a PSP, but let's not report it.
			return hleLogDebug(Log::sceSas, SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE, "sceSasSetADSRMode(%08x, %i, %i, %08x, %08x, %08x, %08x): invalid modes", core, voiceNum, flag, a, d, s, r);
		} else {
			WARN_LOG_REPORT(Log::sceSas, "sceSasSetADSRMode(%08x, %i, %i, %08x, %08x, %08x, %08x): invalid modes", core, voiceNum, flag, a, d, s, r);
			return hleNoLog(SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE);
		}
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	v.envelope.SetEnvelope(flag, a, d, s, r);
	return hleLogDebug(Log::sceSas, 0);
}


static u32 sceSasSetSimpleADSR(u32 core, int voiceNum, u32 ADSREnv1, u32 ADSREnv2) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	}
	// This bit could be related to decay type or systain type, but gives an error if you try to set it.
	if ((ADSREnv2 >> 13) & 1) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE, "Invalid ADSREnv2");
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	v.envelope.SetSimpleEnvelope(ADSREnv1 & 0xFFFF, ADSREnv2 & 0xFFFF);
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasGetEnvelopeHeight(u32 core, int voiceNum) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voiceNum");
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	int height = v.envelope.GetHeight();
	return hleLogDebug(Log::sceSas, height);
}

static u32 sceSasRevType(u32 core, int type) {
	if (type < PSP_SAS_EFFECT_TYPE_OFF || type > PSP_SAS_EFFECT_TYPE_MAX) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_REV_INVALID_TYPE, "invalid type");
	}

	__SasDrain();
	sas->SetWaveformEffectType(type);
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasRevParam(u32 core, int delay, int feedback) {
	if (delay < 0 || delay >= 128) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_REV_INVALID_DELAY, "invalid delay value");
	}
	if (feedback < 0 || feedback >= 128) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_REV_INVALID_FEEDBACK, "invalid feedback value");
	}

	__SasDrain();
	sas->waveformEffect.delay = delay;
	sas->waveformEffect.feedback = feedback;
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasRevEVOL(u32 core, u32 lv, u32 rv) {
	if (lv > 0x1000 || rv > 0x1000) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_REV_INVALID_VOLUME, "invalid volume");
	}

	__SasDrain();
	sas->waveformEffect.leftVol = lv;
	sas->waveformEffect.rightVol = rv;
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasRevVON(u32 core, int dry, int wet) {
	__SasDrain();
	sas->waveformEffect.isDryOn = dry != 0;
	sas->waveformEffect.isWetOn = wet != 0;
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasGetGrain(u32 core) {
	return hleLogDebug(Log::sceSas, sas->GetGrainSize());
}

static u32 sceSasSetGrain(u32 core, int grain) {
	__SasDrain();
	sas->SetGrainSize(grain);
	return hleLogInfo(Log::sceSas, 0);
}

static u32 sceSasGetOutputMode(u32 core) {
	return hleLogDebug(Log::sceSas, sas->outputMode);
}

static u32 sceSasSetOutputMode(u32 core, u32 outputMode) {
	if (outputMode != 0 && outputMode != 1) {
		ERROR_LOG_REPORT(Log::sceSas, "sceSasSetOutputMode(%08x, %i): bad output mode", core, outputMode);
		return hleNoLog(SCE_SAS_ERROR_INVALID_OUTPUT_MODE);
	}

	__SasDrain();
	sas->outputMode = outputMode;
	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasGetAllEnvelopeHeights(u32 core, u32 heightsAddr) {
	if (!Memory::IsValidAddress(heightsAddr)) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_INVALID_PARAMETER);
	}

	__SasDrain();
	for (int i = 0; i < PSP_SAS_VOICES_MAX; i++) {
		int voiceHeight = sas->voices[i].envelope.GetHeight();
		Memory::Write_U32(voiceHeight, heightsAddr + i * 4);
	}

	return hleLogDebug(Log::sceSas, 0);
}

static u32 sceSasSetTriangularWave(u32 sasCore, int voice, int unknown) {
	ERROR_LOG_REPORT(Log::sceSas, "UNIMPL sceSasSetTriangularWave(%08x, %i, %i)", sasCore, voice, unknown);
	return hleNoLog(0);
}

static u32 sceSasSetSteepWave(u32 sasCore, int voice, int unknown) {
	ERROR_LOG_REPORT(Log::sceSas, "UNIMPL sceSasSetSteepWave(%08x, %i, %i)", sasCore, voice, unknown);
	return hleNoLog(0);
}

static u32 __sceSasSetVoiceATRAC3(u32 core, int voiceNum, u32 atrac3Context) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voicenum");
	}

	__SasDrain();
	SasVoice &v = sas->voices[voiceNum];
	if (v.type == VOICETYPE_ATRAC3) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_ATRAC3_ALREADY_SET, "voice is already ATRAC3");
	}
	v.type = VOICETYPE_ATRAC3;
	v.loop = false;
	v.playing = true;
	v.atrac3.SetContext(atrac3Context);
	Memory::Write_U32(atrac3Context, core + 56 * voiceNum + 20);
	return hleLogDebug(Log::sceSas, 0);
}

static u32 __sceSasConcatenateATRAC3(u32 core, int voiceNum, u32 atrac3DataAddr, int atrac3DataLength) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voicenum");
	}

	__SasDrain();

	DEBUG_LOG_REPORT_ONCE(concatAtrac3, Log::sceSas, "__sceSasConcatenateATRAC3(%08x, %i, %08x, %i)", core, voiceNum, atrac3DataAddr, atrac3DataLength);

	SasVoice &v = sas->voices[voiceNum];
	v.atrac3.Concatenate(atrac3DataAddr, atrac3DataLength);
	return hleLogDebug(Log::sceSas, 0);
}

static u32 __sceSasUnsetATRAC3(u32 core, int voiceNum) {
	if (voiceNum >= PSP_SAS_VOICES_MAX || voiceNum < 0) {
		return hleLogWarning(Log::sceSas, SCE_SAS_ERROR_INVALID_VOICE, "invalid voicenum");
	}

	__SasDrain();

	SasVoice &v = sas->voices[voiceNum];
	if (v.type != VOICETYPE_ATRAC3) {
		return hleLogError(Log::sceSas, SCE_SAS_ERROR_ATRAC3_NOT_SET, "voice is not ATRAC3");
	}
	v.type = VOICETYPE_OFF;
	v.playing = false;
	v.on = false;
	// This unpauses.  Some games, like Sol Trigger, depend on this.
	v.paused = false;
	Memory::Write_U32(0, core + 56 * voiceNum + 20);

	return hleLogDebug(Log::sceSas, 0);
}

void __SasGetDebugStats(char *stats, size_t bufsize) {
	if (sas) {
		sas->GetDebugText(stats, bufsize);
	} else {
		snprintf(stats, bufsize, "Sas not initialized");
	}
}

const HLEFunction sceSasCore[] = {
	{0X42778A9F, &WrapU_UUUUU<sceSasInit>,               "__sceSasInit",                  'x', "xxxxx"  },
	{0XA3589D81, &WrapU_UU<_sceSasCore>,                 "__sceSasCore",                  'x', "xx"     },
	{0X50A14DFC, &WrapU_UUII<_sceSasCoreWithMix>,        "__sceSasCoreWithMix",           'x', "xxii"   },
	{0X68A46B95, &WrapU_U<sceSasGetEndFlag>,             "__sceSasGetEndFlag",            'x', "x"      },
	{0X440CA7D8, &WrapU_UIIIII<sceSasSetVolume>,         "__sceSasSetVolume",             'x', "xiiiii" },
	{0XAD84D37F, &WrapU_UII<sceSasSetPitch>,             "__sceSasSetPitch",              'x', "xii"    },
	{0X99944089, &WrapU_UIUII<sceSasSetVoice>,           "__sceSasSetVoice",              'x', "xixii"  },
	{0XB7660A23, &WrapU_UII<sceSasSetNoise>,             "__sceSasSetNoise",              'x', "xii"    },
	{0X019B25EB, &WrapU_UIIIIII<sceSasSetADSR>,          "__sceSasSetADSR",               'x', "xiiiiii"},
	{0X9EC3676A, &WrapU_UIIIIII<sceSasSetADSRMode>,      "__sceSasSetADSRmode",           'x', "xiiiiii"},
	{0X5F9529F6, &WrapU_UII<sceSasSetSL>,                "__sceSasSetSL",                 'x', "xii"    },
	{0X74AE582A, &WrapU_UI<sceSasGetEnvelopeHeight>,     "__sceSasGetEnvelopeHeight",     'x', "xi"     },
	{0XCBCD4F79, &WrapU_UIUU<sceSasSetSimpleADSR>,       "__sceSasSetSimpleADSR",         'x', "xixx"   },
	{0XA0CF2FA4, &WrapU_UI<sceSasSetKeyOff>,             "__sceSasSetKeyOff",             'x', "xi"     },
	{0X76F01ACA, &WrapU_UI<sceSasSetKeyOn>,              "__sceSasSetKeyOn",              'x', "xi"     },
	{0XF983B186, &WrapU_UII<sceSasRevVON>,               "__sceSasRevVON",                'x', "xii"    },
	{0XD5A229C9, &WrapU_UUU<sceSasRevEVOL>,              "__sceSasRevEVOL",               'x', "xxx"    },
	{0X33D4AB37, &WrapU_UI<sceSasRevType>,               "__sceSasRevType",               'x', "xi"     },
	{0X267A6DD2, &WrapU_UII<sceSasRevParam>,             "__sceSasRevParam",              'x', "xii"    },
	{0X2C8E6AB3, &WrapU_U<sceSasGetPauseFlag>,           "__sceSasGetPauseFlag",          'x', "x"      },
	{0X787D04D5, &WrapU_UUI<sceSasSetPause>,             "__sceSasSetPause",              'x', "xxi"    },
	{0XA232CBE6, &WrapU_UII<sceSasSetTriangularWave>,    "__sceSasSetTrianglarWave",      'x', "xii"    }, // Typo.
	{0XD5EBBBCD, &WrapU_UII<sceSasSetSteepWave>,         "__sceSasSetSteepWave",          'x', "xii"    },
	{0XBD11B7C2, &WrapU_U<sceSasGetGrain>,               "__sceSasGetGrain",              'x', "x"      },
	{0XD1E0A01E, &WrapU_UI<sceSasSetGrain>,              "__sceSasSetGrain",              'x', "xi"     },
	{0XE175EF66, &WrapU_U<sceSasGetOutputMode>,          "__sceSasGetOutputmode",         'x', "x"      },
	{0XE855BF76, &WrapU_UU<sceSasSetOutputMode>,         "__sceSasSetOutputmode",         'x', "xx"     },
	{0X07F58C24, &WrapU_UU<sceSasGetAllEnvelopeHeights>, "__sceSasGetAllEnvelopeHeights", 'x', "xx"     },
	{0XE1CD9561, &WrapU_UIUII<sceSasSetVoicePCM>,        "__sceSasSetVoicePCM",           'x', "xixii"  },
	{0X4AA9EAD6, &WrapU_UIU<__sceSasSetVoiceATRAC3>,     "__sceSasSetVoiceATRAC3",        'x', "xix"    },
	{0X7497EA85, &WrapU_UIUI<__sceSasConcatenateATRAC3>, "__sceSasConcatenateATRAC3",     'x', "xixi"   },
	{0XF6107F00, &WrapU_UI<__sceSasUnsetATRAC3>,         "__sceSasUnsetATRAC3",           'x', "xi"     },
};

void Register_sceSasCore()
{
	RegisterHLEModule("sceSasCore", ARRAY_SIZE(sceSasCore), sceSasCore);
}

