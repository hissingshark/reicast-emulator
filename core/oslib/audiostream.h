#pragma once
#include "types.h"
#include <tuple>

//Get used size in the ring buffer
u32 asRingUsedCount();
//Get free size in the ring buffer
u32 asRingFreeCount();
//Read 'Count' samples from the ring buffer.Returns true if successful.
//If sz==0 then a whole buffer is read
bool asRingRead(u8* dst,u32 count=0);
void UpdateBuff(u8* pos);


typedef std::vector<std::string> (*audio_option_callback_t)();
enum audio_option_type
{
	integer = 0
,	checkbox = 1
,	list = 2
};

typedef struct {
	std::string cfg_name;
	std::string caption;
	audio_option_type type;

	// type int_value (spin edit)
	int min_value;
	int max_value;

	// type list edit (string/char*)
	audio_option_callback_t list_callback;
} audio_option_t;

typedef audio_option_t* (*audio_options_func_t)(int* option_count);


typedef u32(*audio_backend_pull_callback_t)(void* buffer, u32 buffer_size, u32 amt, u32 target_rate);
typedef void (*audio_backend_init_func_t)(audio_backend_pull_callback_t pull_callback);
typedef u32 (*audio_backend_push_func_t)(void*, u32, bool);
typedef void (*audio_backend_term_func_t)();
typedef bool (*audio_backend_prefer_pull_func_t)();
typedef struct {
    string slug;
    string name;
    audio_backend_init_func_t init;
    audio_backend_push_func_t push;
    audio_backend_term_func_t term;
	audio_options_func_t get_options;
	audio_backend_prefer_pull_func_t prefer_pull;
} audiobackend_t;
extern bool RegisterAudioBackend(audiobackend_t* backend);
extern void InitAudio();
extern u32 PushAudio(void* frame, u32 amt, bool wait);
extern void TermAudio();

u32 GetAudioBackendCount();
void SortAudioBackends();
audiobackend_t* GetAudioBackend(int num);
audiobackend_t* GetAudioBackend(std::string slug);
