#include <limits.h>
#include "cfg/cfg.h"
#include "oslib/oslib.h"
#include "audiostream.h"
#ifndef _WIN32
#include <unistd.h>
#endif

struct SoundFrame { s16 l; s16 r; };
#define SAMPLE_COUNT 4096
#define PUSH_COUNT 512
#define OVERRUN_SKIP 128

u32 gen_samples = 0;

double time_diff = 128 / 44100.0;
double time_last;

#ifdef LOG_SOUND
// TODO Only works on Windows!
WaveWriter rawout("d:\\aica_out.wav");
#endif

static unsigned int audiobackends_num_max = 1;
static unsigned int audiobackends_num_registered = 0;
static audiobackend_t **audiobackends = NULL;
static audiobackend_t *audiobackend_current = NULL;

u32 SmallCpuWait()
{
#if defined(_WIN32)
	Sleep(1);
#else // if linux?
	usleep(1000);
#endif
	return 0;
}

static float InterpolateCatmull4pt3oX(float x0, float x1, float x2, float x3, float t) {
	return 0.45f* ((2 * x1) + t * ((-x0 + x2) + t * ((2 * x0 - 5 * x1 + 4 * x2 - x3) + t * (-x0 + 3 * x1 - 3 * x2 + x3))));
}

static s16 SaturateToS16(float v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (s16)v;
}

class PullBuffer_t {
private:
	SoundFrame RingBuffer[SAMPLE_COUNT];
	const u32 RingBufferByteSize = sizeof(RingBuffer);
	const u32 RingBufferSampleCount = SAMPLE_COUNT;

	volatile u32 WritePtr;  //last WRITEN sample
	volatile u32 ReadPtr;   //next sample to read

	u32 asRingUsedCount()
	{
		if (WritePtr > ReadPtr)
			return WritePtr - ReadPtr;
		else
			return RingBufferSampleCount - (ReadPtr - WritePtr);
		//s32 sz=(WritePtr+1)%RingBufferSampleCount-ReadPtr;
		//return sz<0?sz+RingBufferSampleCount:sz;
	}

	u32 asRingFreeCount()
	{
		return RingBufferSampleCount - asRingUsedCount();
	}

	SoundFrame Status[4] = { 0 };
	float current_partial_pos = 0;
public:
	u32 ReadAudioResampling(void* buffer, u32 buffer_size, u32 amt, u32 target_rate)
	{
		if (buffer == nullptr)
			return asRingUsedCount() * target_rate / 44100;

		SoundFrame* outbuf = (SoundFrame*)buffer;

		float inc = 44100.0f / target_rate;

		for (int i = 0; i < amt; i++)
		{
			SoundFrame res = {
				SaturateToS16(InterpolateCatmull4pt3oX(Status[0].l,Status[1].l,Status[2].l,Status[3].l,current_partial_pos)),
				SaturateToS16(InterpolateCatmull4pt3oX(Status[0].r,Status[1].r,Status[2].r,Status[3].r,current_partial_pos))
			};
			outbuf[i] = res;
			current_partial_pos += inc;
			while (current_partial_pos >= 1)
			{
				Status[0] = Status[1];
				Status[1] = Status[2];
				Status[2] = Status[3];
				Status[3] = RingBuffer[ReadPtr];
				ReadPtr = (ReadPtr + 1) % RingBufferSampleCount;
				current_partial_pos -= 1;
			}
		}
	}

	u32 ReadAudio(void* buffer, u32 buffer_size, u32 amt)
	{
		if (buffer == nullptr)
			return asRingUsedCount();

		u32 read = min(min(amt, buffer_size), asRingUsedCount());
		u32 first = min(RingBufferSampleCount - ReadPtr, read);
		memcpy(buffer, RingBuffer + ReadPtr, first * sizeof(SoundFrame));
		if (read > first)
		{
			memcpy(((SoundFrame*)buffer) + first, RingBuffer,(read - first) * sizeof(SoundFrame));
		}
		ReadPtr = (ReadPtr + read) % RingBufferSampleCount;
		return read;
	}

	u32 PushAudio(u32 amt, bool wait)
	{
		static u32 buffer[PUSH_COUNT];
		if (audiobackend_current != NULL) {
			ReadAudio(buffer, PUSH_COUNT, amt);
			return audiobackend_current->push(buffer, amt, wait);
		}
		return 0;
	}

	void WriteSample(s16 r, s16 l, bool wait)
	{
		const u32 ptr = (WritePtr + 1) % RingBufferSampleCount;
		RingBuffer[ptr].r = r;
		RingBuffer[ptr].l = l;
		WritePtr = ptr;

		if (asRingFreeCount() < 16)
		{
			if (wait)
			{
				// infinite?
				while (asRingFreeCount() < 512)
					SmallCpuWait();
			}
			else
			{
				ReadPtr = (ReadPtr + OVERRUN_SKIP) % RingBufferSampleCount;
			}
		}
	}
};

class PushBuffer_t {
private:
	SoundFrame RingBuffer[PUSH_COUNT];
	const u32 RingBufferByteSize = sizeof(RingBuffer);
	const u32 RingBufferSampleCount = PUSH_COUNT;

	volatile u32 WritePtr;  //last WRITEN sample

	u32 asRingUsedCount()
	{
		return (WritePtr > 0)
			? WritePtr 
			: RingBufferSampleCount + WritePtr;
	}

public:
	u32 PushAudio(u32 amt, bool wait)
	{
		if (audiobackend_current != NULL) {
			return audiobackend_current->push(RingBuffer, amt, wait);
		}
		return 0;
	}

	void WriteSample(s16 r, s16 l, bool wait)
	{
		const u32 ptr = (WritePtr + 1) % RingBufferSampleCount;
		RingBuffer[ptr].r = r;
		RingBuffer[ptr].l = l;
		WritePtr = ptr;

		u32 used = asRingUsedCount();
		if (used >= PUSH_COUNT)
		{
			PushAudio(PUSH_COUNT, wait);
		}
	}
};

static PullBuffer_t PullBuffer;
static PushBuffer_t PushBuffer;

u32 PullAudioCallback(void* buffer, u32 buffer_size, u32 amt, u32 target_rate)
{
	if (target_rate != 0 && target_rate != 44100)
		return PullBuffer.ReadAudioResampling(buffer, buffer_size, amt, target_rate);
	else
		return PullBuffer.ReadAudio(buffer, buffer_size, amt);
}

u32 GetAudioBackendCount()
{
	return audiobackends_num_registered;
}

audiobackend_t* GetAudioBackend(int num)
{
	return audiobackends[num];
}

bool RegisterAudioBackend(audiobackend_t *backend)
{
	/* This function announces the availability of an audio backend to reicast. */
	// Check if backend is valid
	if (backend == NULL)
	{
		printf("ERROR: Tried to register invalid audio backend (NULL pointer).\n");
		return false;
	}

	if (backend->slug == "auto" || backend->slug == "none") {
		printf("ERROR: Tried to register invalid audio backend (slug \"%s\" is a reserved keyword).\n", backend->slug.c_str());
		return false;
	}

	// First call to RegisterAudioBackend(), create the backend structure;
	if (audiobackends == NULL)
		audiobackends = static_cast<audiobackend_t**>(calloc(audiobackends_num_max, sizeof(audiobackend_t*)));

	// Check if we need to allocate addition memory for storing the pointers and allocate if neccessary
	if (audiobackends_num_registered == audiobackends_num_max)
	{
		// Check for integer overflows
		if (audiobackends_num_max == UINT_MAX)
		{
			printf("ERROR: Registering audio backend \"%s\" (%s) failed. Cannot register more than %u backends\n", backend->slug.c_str(), backend->name.c_str(), audiobackends_num_max);
			return false;
		}
		audiobackends_num_max++;
		audiobackend_t **new_ptr = static_cast<audiobackend_t**>(realloc(audiobackends, audiobackends_num_max * sizeof(audiobackend_t*)));
		// Make sure that allocation worked
		if (new_ptr == NULL)
		{
			printf("ERROR: Registering audio backend \"%s\" (%s) failed. Cannot allocate additional memory.\n", backend->slug.c_str(), backend->name.c_str());
			return false;
		}
		audiobackends = new_ptr;
	}

	audiobackends[audiobackends_num_registered] = backend;
	audiobackends_num_registered++;
	return true;
}

audiobackend_t* GetAudioBackend(std::string slug)
{
	if (slug == "none")
	{
		printf("WARNING: Audio backend set to \"none\"!\n");
	}
	else if (audiobackends_num_registered > 0)
	{
		if (slug == "auto")
		{
			/* FIXME: At some point, one might want to insert some intelligent
				 algorithm for autoselecting the approriate audio backend here.
				 I'm too lazy right now. */
			printf("Auto-selected audio backend \"%s\" (%s).\n", audiobackends[0]->slug.c_str(), audiobackends[0]->name.c_str());
			return audiobackends[0];
		}
		else
		{
			for (unsigned int i = 0; i < audiobackends_num_registered; i++)
			{
				if (audiobackends[i]->slug == slug)
				{
					return audiobackends[i];
				}
			}
			printf("WARNING: Audio backend \"%s\" not found!\n", slug.c_str());
		}
	}
	else
	{
		printf("WARNING: No audio backends available!\n");
	}
	return NULL;
}

bool IsPullMode()
{
	if (audiobackend_current != NULL && audiobackend_current->prefer_pull != NULL) {
		return audiobackend_current->prefer_pull();
	}
	return false;
}

extern double mspdf;
void WriteSample(s16 r, s16 l)
{
	bool wait = settings.aica.LimitFPS && (mspdf <= 11);
	
	if (IsPullMode())
	{
		PullBuffer.WriteSample(r, l, wait);
	}
	else
	{
		PushBuffer.WriteSample(r, l, wait);
	}
}

static bool backends_sorted = false;
void SortAudioBackends()
{
	if (backends_sorted)
		return;

	// Sort backends by slug
	for (int n = audiobackends_num_registered; n > 0; n--)
	{
		for (int i = 0; i < n - 1; i++)
		{
			if (audiobackends[i]->slug > audiobackends[i + 1]->slug)
			{
				audiobackend_t* swap = audiobackends[i];
				audiobackends[i] = audiobackends[i + 1];
				audiobackends[i + 1] = swap;
			}
		}
	}
}

void InitAudio()
{
	if (cfgLoadInt("audio", "disable", 0)) {
		printf("WARNING: Audio disabled in config!\n");
		return;
	}

	cfgSaveInt("audio", "disable", 0);

	if (audiobackend_current != NULL) {
		printf("ERROR: The audio backend \"%s\" (%s) has already been initialized, you need to terminate it before you can call audio_init() again!\n", audiobackend_current->slug.c_str(), audiobackend_current->name.c_str());
		return;
	}

	SortAudioBackends();

	string audiobackend_slug = settings.audio.backend;
	audiobackend_current = GetAudioBackend(audiobackend_slug);
	if (audiobackend_current == NULL) {
		printf("WARNING: Running without audio!\n");
		return;
	}

	printf("Initializing audio backend \"%s\" (%s)...\n", audiobackend_current->slug.c_str(), audiobackend_current->name.c_str());
	audiobackend_current->init(PullAudioCallback);
}

void TermAudio()
{
	if (audiobackend_current != NULL) {
		audiobackend_current->term();
		printf("Terminating audio backend \"%s\" (%s)...\n", audiobackend_current->slug.c_str(), audiobackend_current->name.c_str());
		audiobackend_current = NULL;
	}
}
